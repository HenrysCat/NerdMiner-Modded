#include "mailNotifier.h"
#include "mailPolicy.h"
#include "memoryDiagnostics.h"
#include "gmailRoots.h"
#include "mining.h"
#include "monitor.h"
#include "wManager.h"
#include "timeutil.h"
#include "drivers/storage/storage.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>
#include <sys/time.h>
#include <atomic>
#include <new>
#include <errno.h>
#include <stdio.h>
#include <lwip/sockets.h>

extern TSettings Settings;
// Implemented next to the ring buffers; called only on the Arduino loop.
extern void mailAppendHistoryStats(String &body);

namespace {
enum Kind { Daily=0, Test=1, Warning=2, Recovery=3 };
struct Event {
  bool active=false;
  Kind kind=Daily;
  int day=0;
  uint64_t occurred=0, next=0;
  uint32_t duration=0;
  unsigned attempts=0;
};
// Slots 0/1 reserved for report/test; four bounded incident slots.
Event events[6];
Preferences prefs;
bool initialized=false,busy=false,dirty=false,enabledBefore=false;
bool down=false,warned=false,recovering=false;
uint64_t downAt=0,recoverAt=0,nextSend=0,lastTick=0;
uint32_t incidents=0,dropped=0;
int sentDay=0,pendingDay=0,currentJob=-1;
uint64_t dailyRetryUTC=0;
std::atomic<int> result{0};
enum MailPhase { Idle, Preparing, Resolving, WaitingSHA, Connecting, SpeakingSMTP, SendingData, Closing, Finished };
std::atomic<unsigned> phase{Idle};
std::atomic<uint32_t> phaseSince{0};
std::atomic<bool> gmailAccepted{false};
void markPhase(MailPhase value) {
  phaseSince.store((uint32_t)(esp_timer_get_time()/1000000ULL));
  phase.store(value);
}
struct MailShaRequest {
  MailShaRequest(){g_mailShaRequested.store(true);}
  ~MailShaRequest(){g_mailShaRequested.store(false);}
};
void closeMailClient(WiFiClientSecure &client) {
  markPhase(Closing);
  // The final DATA response decides acceptance, not QUIT or a FIN exchange.
  // Closing must not hold the SHA engine indefinitely waiting for TCP FIN.
#ifdef SO_LINGER
  const int socket=client.fd();
  if(socket>=0){linger immediate{1,0};setsockopt(socket,SOL_SOCKET,SO_LINGER,&immediate,sizeof(immediate));}
#endif
  client.stop();
}

// Worker writes this bounded buffer before publishing the atomic result.
// Only complete() reads it, after acquiring that result. No credentials.
char transportDiagnostic[640] = {};
String status="E-Mail ausgeschaltet";
struct Job { String from,to,password,subject,body; time_t utc; };

bool networkConnected() {
  wifi_ap_record_t accessPoint{};
  const bool associated=esp_wifi_sta_get_ap_info(&accessPoint)==ESP_OK;
  const bool hasIP=(uint32_t)WiFi.localIP()!=0;
  return mailNetworkReady(associated,hasIP);
}
uint64_t secondsNow() { return esp_timer_get_time()/1000000ULL; }
void queueIncident(Kind kind,uint64_t now,uint32_t duration) {
  for(unsigned i=2;i<6;++i) if(!events[i].active) {
    events[i]=Event{};events[i].active=true;events[i].kind=kind;
    events[i].occurred=now;events[i].duration=duration;return;
  }
  ++dropped;
}
void clearEvents() {
  for(auto &e:events)e=Event{};
  down=false;warned=false;recovering=false;
}
void ensurePrefs() {
  if(initialized)return;
  initialized=prefs.begin("nm-mail",false);
  if(!initialized){status="E-Mail-Speicher nicht verfuegbar";return;}
  sentDay=prefs.getInt("sent",0);pendingDay=prefs.getInt("pending",0);
  dailyRetryUTC=prefs.getULong64("retry",0);
  if(pendingDay>sentDay) {
    events[0].active=true;events[0].kind=Daily;events[0].day=pendingDay;
    events[0].attempts=prefs.getUInt("tries",0);
    events[0].next=secondsNow()+60; // no immediate retry loop after reset
  }
}
// SMTP line parser: bounded memory and one shared deadline for the session.
bool reply(WiFiClientSecure &client,int wanted,uint64_t deadline) {
  char line[513];size_t n=0;unsigned lines=0;
  while(secondsNow()<deadline && lines<32) {
    while(client.available()) {
      int c=client.read();if(c<0)break;
      if(c=='\n') {
        if(n && line[n-1]=='\r')--n;line[n]=0;
        if(n<4 || mailReplyCode(line)!=wanted)return false;
        if(line[3]==' ')return true;
        n=0;++lines;
      } else {if(n>=sizeof(line)-1)return false;line[n++]=(char)c;}
    }
    if(!client.connected() && !client.available())return false;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return false;
}
bool command(WiFiClientSecure &c,const String &text,int code,uint64_t deadline) {
  if(secondsNow()>=deadline)return false;
  const String line=text+"\r\n";
  return c.write((const uint8_t*)line.c_str(),line.length())==line.length() && reply(c,code,deadline);
}
String base64(const String &text) {
  char out[192];size_t used=0;
  if(mbedtls_base64_encode((unsigned char*)out,sizeof(out)-1,&used,
      (const unsigned char*)text.c_str(),text.length())!=0)return String();
  out[used]=0;return String(out);
}

String networkDetails() {
  return "IP "+WiFi.localIP().toString()+", GW "+WiFi.gatewayIP().toString()+
    ", Maske "+WiFi.subnetMask().toString()+", DNS "+WiFi.dnsIP().toString();
}

int deliver(const Job &j) {
  MemoryProbe memory(MemoryPoint::MailBegin, MemoryPoint::MailEnd);
  timeval tv{j.utc,0};settimeofday(&tv,nullptr); // valid UTC required by certificate checks
  transportDiagnostic[0]=0;
  markPhase(Resolving);
  IPAddress smtpIP;
  if(!WiFi.hostByName("smtp.gmail.com",smtpIP) || (uint32_t)smtpIP==0) {
    snprintf(transportDiagnostic,sizeof(transportDiagnostic),
      "DNS-Aufloesung smtp.gmail.com fehlgeschlagen; DNS-Server %s",
      WiFi.dnsIP().toString().c_str());
    return -6;
  }
  const String route=networkDetails();
  WiFiClientSecure client;client.setCACert(GMAIL_ROOTS);
  client.setHandshakeTimeout(8);client.setTimeout(2); // seconds in this core
  // All TLS operations can use hardware SHA. Share the exact mining lock.
  markPhase(WaitingSHA);
  MailShaRequest mailRequest;
  HW_SHA_REQUEST();std::unique_lock<std::mutex> lock(g_hwShaMutex,std::defer_lock);
  const uint64_t lockLimit=secondsNow()+20;
  while(!lock.try_lock()) {
    if(secondsNow()>=lockLimit) {
      snprintf(transportDiagnostic,sizeof(transportDiagnostic),"SHA-Hardware innerhalb von 20 Sekunden nicht frei");
      return -8;
    }
    vTaskDelay(1);
  }
  markPhase(Connecting);
  const uint64_t deadline=secondsNow()+35;
  const uint32_t heapBefore=ESP.getFreeHeap();
  const uint32_t largestBefore=heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  client.setTimeout(8); // socket timeout; connect() below explicitly uses 8000 ms
  errno=0;
  // Use the same single hostname-based connection as the working miner.
  // setCACert above preserves trust/hostname verification. The preliminary
  // DNS lookup is diagnostic only; the client owns actual target selection.
  if(!client.connect("smtp.gmail.com",465,8000)) {
    const int socketError=errno;
    char detail[140]={};const int tlsError=client.lastError(detail,sizeof(detail));
    snprintf(transportDiagnostic,sizeof(transportDiagnostic),
      "Einzelverbindung smtp.gmail.com:465 (vorab aufgeloest: %s); TLS-Code %d; %s; Socket errno %d; RAM vor TLS %lu KiB, groesster Block %lu KiB; %s",
      smtpIP.toString().c_str(),tlsError,detail,socketError,
      (unsigned long)(heapBefore/1024),(unsigned long)(largestBefore/1024),route.c_str());
    closeMailClient(client);return -1;
  }
  client.setTimeout(2); // bounded SMTP writes after connection is established
  markPhase(SpeakingSMTP);
  if(!reply(client,220,deadline) || !command(client,"EHLO nerdminer.local",250,deadline)) {closeMailClient(client);return -2;}
  if(!command(client,"AUTH LOGIN",334,deadline) ||
     !command(client,base64(j.from),334,deadline) ||
     !command(client,base64(j.password),235,deadline)) {closeMailClient(client);return -3;}
  if(!command(client,"MAIL FROM:<"+j.from+">",250,deadline) ||
     !command(client,"RCPT TO:<"+j.to+">",250,deadline) ||
     !command(client,"DATA",354,deadline)) {closeMailClient(client);return -4;}
  // All mail fields are ASCII. Addresses are validated; user-supplied text
  // never enters subject/body. Dot-stuffing is still applied defensively.
  String body=j.body;body.replace("\r","");body.replace("\n.","\n..");
  if(body.startsWith("."))body="."+body;
  body.replace("\n","\r\n");
  char date[64];tm utc;gmtime_r(&j.utc,&utc);
  strftime(date,sizeof(date),"%a, %d %b %Y %H:%M:%S +0000",&utc);
  markPhase(SendingData);
  String message="From: NerdMiner <"+j.from+">\r\nTo: <"+j.to+">\r\nDate: "+date+
    "\r\nSubject: "+j.subject+"\r\nMIME-Version: 1.0\r\nContent-Type: text/plain; charset=us-ascii\r\nContent-Transfer-Encoding: 7bit\r\n\r\n"+body+"\r\n.\r\n";
  if(secondsNow()>=deadline || client.write((const uint8_t*)message.c_str(),message.length())!=message.length()) {closeMailClient(client);return -5;}
  if(!reply(client,250,deadline)) {closeMailClient(client);return -5;}
  // A final 250 means Gmail accepted the mail. QUIT failure must not cause
  // another copy after acceptance. No SMTP response text is ever logged.
  gmailAccepted.store(true);
  closeMailClient(client);return 1;
}
void worker(void *arg) {
  Job *job=static_cast<Job*>(arg);
  const int done=deliver(*job);
  // Only the immutable snapshot belongs to this task. Settings, queue,
  // Preferences and status strings are owned by the Arduino loop.
  job->password="";delete job;
  markPhase(Finished);result.store(done);vTaskDelete(nullptr);
}
const char *failure(int r) {
  switch(r) {
    case -8:return "Mailversand wartet auf SHA-Hardware";
    case -6:return "Gmail-DNS fehlgeschlagen";
    case -1:return "Gmail-Verbindung/TLS fehlgeschlagen";
    case -2:return "Gmail-SMTP-Begruessung fehlgeschlagen";
    case -3:return "Gmail-Anmeldung fehlgeschlagen: App-Passwort pruefen";
    case -4:return "Gmail hat Absender/Empfaenger nicht angenommen";
    default:return "Keine eindeutige Gmail-Bestaetigung; Wiederholung kann Duplikat erzeugen";
  }
}
void complete(uint64_t now) {
  const int r=result.exchange(0);if(!r)return;
  busy=false;Event &e=events[currentJob];
  scheduleMemoryMailSettled();
  if(r==1) {
    status="Von Gmail angenommen (Zustellung nicht bestaetigt)";
    if(e.kind==Daily) {
      sentDay=e.day;pendingDay=0;
      prefs.putInt("sent",sentDay);prefs.putInt("pending",0);prefs.putUInt("tries",0);
      dailyRetryUTC=0;prefs.putULong64("retry",0);
    }
    e=Event{};
  } else {
    status=failure(r);
    if((r==-1 || r==-6 || r==-8) && transportDiagnostic[0]) {
      status+=" [";status+=transportDiagnostic;status+="]";
      Serial.printf("[MAIL] Verbindung: %s\n",transportDiagnostic);
    }
    e.next=now+900;
    if(e.kind==Daily) {
      time_t utc;if(getUtcTime(utc,false)) {
        dailyRetryUTC=(uint64_t)utc+900;prefs.putULong64("retry",dailyRetryUTC);
      }
    }
    if(e.attempts>=3) {
      status+="; 3 Versuche erreicht";
      if(e.kind!=Daily)e=Event{};
    } else status+="; neuer Versuch fruehestens in 15 Minuten";
  }
  currentJob=-1;nextSend=now+120;
}
String jobBody(const Event &e,const MinerStatus &m,time_t utc,uint64_t now) {
  String body;body.reserve(2200);
  body="NerdMiner - ";
  if(e.kind==Daily)body+="Statistikbericht, geplant fuer "+String(e.day)+" um 20:00 Europe/Berlin\n";
  else if(e.kind==Test)body+="Testmail\n";
  else if(e.kind==Warning)body+="Pool-Unterbrechung erkannt (mindestens 2 Minuten)\n";
  else body+="Pool-Verbindung wieder seit mindestens 30 Sekunden stabil\n";
  const time_t berlin=utc+3600+(tu_euSummerTime(utc)?3600:0);
  tm local;gmtime_r(&berlin,&local);char stamp[40];
  strftime(stamp,sizeof(stamp),"%d.%m.%Y %H:%M:%S",&local);
  body+="Datenstand: "+String(stamp)+" Europe/Berlin\n";
  if(e.kind==Warning || e.kind==Recovery)
    body+="Ereignis vor "+String((unsigned long)(now-e.occurred))+" s; Dauer bei Ereigniserfassung: "+String(e.duration)+" s\n";
  const bool healthy=networkConnected() && m.subscribed && m.authorized==1;
  body+="Pool-Status JETZT: "+String(healthy?"verbunden und angemeldet":"nicht bereit / unterbrochen")+"\n";
  body+="Hashrate aktuell: "+String(m.hashrateKHs,1)+" KH/s\n";
  body+="Durchschnitt seit Firmwarestart: "+String(m.avgHashrateKHs,1)+" KH/s\n";
  body+="Shares seit Firmwarestart: "+String(m.accepted)+" angenommen, "+String(m.rejected)+" abgelehnt\n";
  body+="Beste Difficulty: "+String(getBestDiff(),6)+"\n";
  body+="Laufzeit: "+String((unsigned long)now)+" s; freies RAM vor Versand: "+String(ESP.getFreeHeap())+" Byte\n";
  body+="Pool-Unterbrechungen >=2 min seit Mail-Aktivierung/Neustart: "+String(incidents)+"\n";
  body+="Wegen voller Ereigniswarteschlange ausgelassene Meldungen: "+String(dropped)+"\n\n";
  mailAppendHistoryStats(body);
  body+="\nVerlaeufe nur im RAM seit Neustart. 6h/24h: Min/Max beziehen sich auf Mittelwerte.\n";
  body+="Bei WLAN-/Internetausfall wird erst nach Wiederverbindung gesendet. Ausgeschaltet kein Versand.\n";
  return body;
}
}

bool mailAddressValid(const String &a) {
  if(a.length()<6 || a.length()>100)return false;
  const int at=a.indexOf('@');if(at<=0 || a.lastIndexOf('@')!=at || a.indexOf('.',at+2)<0 || a.endsWith("."))return false;
  for(size_t i=0;i<a.length();++i) {
    const char c=a[i];
    if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='+'||c=='-'||c=='@'))return false;
  }
  return true;
}
bool mailPasswordValid(const String &p) {
  if(p.length()!=16)return false;
  for(size_t i=0;i<p.length();++i)if(!((p[i]>='a'&&p[i]<='z')||(p[i]>='A'&&p[i]<='Z')||(p[i]>='0'&&p[i]<='9')))return false;
  return true;
}
String mailStatusText(){
  if(!Settings.MailEnabled)return "E-Mail ausgeschaltet";
  if(busy) {
    const unsigned current=phase.load();
    const unsigned age=(uint32_t)secondsNow()-phaseSince.load();
    if(gmailAccepted.load())return "Von Gmail angenommen; Versandabschluss wird verarbeitet ("+String(age)+" s)";
    const char *label=current==Resolving?"Gmail-Namen aufloesen":
      current==WaitingSHA?"Warte auf freie SHA-Hardware":
      current==Connecting?"Gmail-Verbindung und TLS aufbauen":
      current==SpeakingSMTP?"Gmail-Anmeldung/SMTP":
      current==SendingData?"Maildaten uebertragen":
      current==Closing?"Mail-Verbindung schliessen":"Mailauftrag vorbereiten";
    return String(label)+" ("+String(age)+" s)";
  }
  if(!mailAddressValid(Settings.MailFrom)||!mailAddressValid(Settings.MailTo)||!mailPasswordValid(Settings.MailPass))
    return "E-Mail-Einstellungen unvollstaendig";
  // Connectivity is always evaluated live. It never overwrites the last
  // SMTP result or an allocation error, even during a backoff interval.
  if(!networkConnected())return "WLAN aktuell nicht verbunden; vorgemerkte Mails warten";
  time_t utc;if(!getUtcTime(utc,false))return "WLAN verbunden; warte auf synchronisierte Uhrzeit (NTP)";
  const uint64_t now=secondsNow();
  unsigned pending=0,exhausted=0;uint64_t earliest=UINT64_MAX;
  for(const auto &e:events)if(e.active) {
    if(e.attempts>=3){++exhausted;continue;}
    ++pending;uint64_t ready=e.next>nextSend?e.next:nextSend;
    if(e.kind==Daily && dailyRetryUTC>(uint64_t)utc) {
      const uint64_t retry=now+dailyRetryUTC-(uint64_t)utc;
      if(retry>ready)ready=retry;
    }
    if(ready<earliest)earliest=ready;
  }
  String text="WLAN verbunden; ";
  if(pending) {
    text+=String(pending)+" Mail(s) vorgemerkt; ";
    const uint64_t wait=earliest>now?earliest-now:0;
    text+=wait?"naechster Versuch fruehestens in "+String((unsigned long)wait)+" s. ":"Versand wird vorbereitet. ";
  } else if(exhausted)text+="Versuchslimit erreicht. ";
  else text+="keine Mail vorgemerkt. ";
  text+="Letzte Meldung: "+status;
  if(status.indexOf("RAM")>=0) {
    text+=" (frei "+String(ESP.getFreeHeap()/1024)+" KiB, groesster Block "+
      String(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)/1024)+" KiB)";
  }
  return text;
}
void mailSettingsChanged(){dirty=true;}
bool mailQueueTest() {
  if(!Settings.MailEnabled || !mailAddressValid(Settings.MailFrom)||!mailAddressValid(Settings.MailTo)||!mailPasswordValid(Settings.MailPass))return false;
  if(events[1].active)return false;
  events[1]=Event{};events[1].active=true;events[1].kind=Test;events[1].occurred=secondsNow();
  status="Testmail vorgemerkt";return true;
}
void mailProcess() {
  const uint64_t now=secondsNow();
  if(busy)complete(now);
  if(lastTick==now)return;lastTick=now;
  if(busy)return; // never mutate the in-flight event
  if(!Settings.MailEnabled) {
    if(enabledBefore){clearEvents();enabledBefore=false;}
    status="E-Mail ausgeschaltet";return;
  }
  ensurePrefs();if(!initialized)return;
  if(!enabledBefore){
    enabledBefore=true;down=false;incidents=0;dropped=0;
    status="E-Mail aktiviert; Bericht um 20:00 Europe/Berlin";
    if(pendingDay>sentDay && !events[0].active) {
      events[0].active=true;events[0].kind=Daily;events[0].day=pendingDay;
      events[0].attempts=prefs.getUInt("tries",0);
    }
  }
  if(dirty) {
    dirty=false;
    for(auto &e:events)if(e.active){e.attempts=0;e.next=now;}
    if(pendingDay){prefs.putUInt("tries",0);dailyRetryUTC=0;prefs.putULong64("retry",0);}
    status="E-Mail aktiviert; Bericht um 20:00 Europe/Berlin";
  }
  if(!mailAddressValid(Settings.MailFrom)||!mailAddressValid(Settings.MailTo)||!mailPasswordValid(Settings.MailPass)) {
    status="E-Mail-Einstellungen unvollstaendig";return;
  }
  const MinerStatus m=getMinerStatus();
  const bool healthy=networkConnected() && m.subscribed && m.authorized==1;
  if(!healthy) {
    recovering=false;
    if(!down){down=true;warned=false;downAt=now;}
    if(mailOutageDue(now,downAt,warned)) {warned=true;++incidents;queueIncident(Warning,now,(uint32_t)(now-downAt));}
  } else if(down) {
    if(!recovering){recovering=true;recoverAt=now;}
    if(mailRecovered(now,recoverAt)) {
      if(warned)queueIncident(Recovery,now,(uint32_t)(recoverAt-downAt));
      down=false;warned=false;recovering=false;
    }
  }
  time_t utc;if(!getUtcTime(utc,false))return;
  const int64_t berlin=(int64_t)utc+3600+(tu_euSummerTime(utc)?3600:0);
  int year;unsigned month,day;tu_civilFromDays(tu_daysOf(berlin),year,month,day);
  const int today=year*10000+month*100+day;
  const unsigned daySeconds=(unsigned)(berlin-tu_daysOf(berlin)*86400);
  if(mailDue(today,daySeconds,sentDay,pendingDay)) {
    // One persistent pending daily report. A newer due day replaces an
    // unsent older report, preventing a backlog of obsolete daily mails.
    pendingDay=today;prefs.putInt("pending",today);prefs.putUInt("tries",0);
    dailyRetryUTC=0;prefs.putULong64("retry",0);
    events[0]=Event{};events[0].active=true;events[0].kind=Daily;events[0].day=today;
  }
  if(!networkConnected())return;
  if(now<nextSend)return;
  int pick=-1;
  // Test, incidents, daily; retry backoff never blocks other ready mail.
  const unsigned order[]={1,2,3,4,5,0};
  for(unsigned i:order)if(events[i].active && mailCanAttempt(events[i].attempts,now,events[i].next)){
    if(events[i].kind==Daily && (uint64_t)utc<dailyRetryUTC)continue;
    pick=i;break;
  }
  if(pick<0)return;
  if(ESP.getFreeHeap()<70000 || heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)<40000) {
    events[pick].next=now+900;status="Zu wenig freier RAM fuer TLS";return;
  }
  Job *job=new(std::nothrow) Job;
  if(!job){events[pick].next=now+900;status="Kein RAM fuer Mailauftrag";return;}
  job->from=Settings.MailFrom;job->to=Settings.MailTo;job->password=Settings.MailPass;
  job->utc=utc;job->body=jobBody(events[pick],m,utc,now);
  job->subject=events[pick].kind==Daily?"NerdMiner Statistik 20:00":events[pick].kind==Test?"NerdMiner Testmail":events[pick].kind==Warning?"NerdMiner Pool-Unterbrechung erkannt":"NerdMiner Pool-Verbindung wiederhergestellt";
  busy=true;currentJob=pick;result.store(0);gmailAccepted.store(false);markPhase(Preparing);
  if(xTaskCreatePinnedToCore(worker,"gmail",12288,job,3,nullptr,0)!=pdPASS) {
    delete job;busy=false;currentJob=-1;events[pick].next=now+900;status="Mail-Task konnte nicht starten";return;
  }
  ++events[pick].attempts;
  if(events[pick].kind==Daily){
    prefs.putUInt("tries",events[pick].attempts);
    dailyRetryUTC=(uint64_t)utc+900;prefs.putULong64("retry",dailyRetryUTC);
  }
  status="Gmail-Versand laeuft; Mining pausiert kurz fuer TLS";
}
