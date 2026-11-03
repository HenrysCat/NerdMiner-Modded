#ifndef NM_MAIL_POLICY_H
#define NM_MAIL_POLICY_H
#include <stdint.h>

// No Arduino dependency: compile-time regression checks can exercise these.
// Station association and a nonzero IP must both be true. Do not trust an
// Arduino WiFi event cache as the only source for the SMTP send gate.
constexpr bool mailNetworkReady(bool associated, bool hasIP) {
  return associated && hasIP;
}
constexpr bool mailSettingsAllowed(bool enabled, bool fromValid, bool toValid, bool passwordValid) {
  return !enabled || (fromValid && toValid && passwordValid);
}
static_assert(mailSettingsAllowed(false,false,false,false), "disabled blank defaults do not block other settings");
static_assert(!mailSettingsAllowed(true,false,true,true), "enabled mail needs sender");
static_assert(!mailSettingsAllowed(true,true,false,true), "enabled mail needs recipient");
static_assert(!mailSettingsAllowed(true,true,true,false), "enabled mail needs app password");
static_assert(mailSettingsAllowed(true,true,true,true), "complete enabled mail");
static_assert(mailNetworkReady(true,true), "associated with IPv4");
static_assert(!mailNetworkReady(false,true), "stale IPv4 after disconnect");
static_assert(!mailNetworkReady(true,false), "waiting for DHCP");
static_assert(!mailNetworkReady(false,false), "offline");
constexpr bool mailDue(int day, unsigned seconds, int sent, int pending) {
  return seconds >= 20U * 3600U && day > sent && day > pending;
}
constexpr bool mailOutageDue(uint64_t now, uint64_t start, bool notified) {
  return !notified && now >= start && now - start >= 120;
}
constexpr bool mailRecovered(uint64_t now, uint64_t stableSince) {
  return now >= stableSince && now - stableSince >= 30;
}
constexpr bool mailCanAttempt(unsigned attempts, uint64_t now, uint64_t next) {
  return attempts < 3 && now >= next;
}
constexpr int mailReplyCode(const char *line) {
  return line[0] >= '1' && line[0] <= '5' && line[1] >= '0' && line[1] <= '9' &&
         line[2] >= '0' && line[2] <= '9' && (line[3] == ' ' || line[3] == '-')
      ? (line[0]-'0')*100+(line[1]-'0')*10+line[2]-'0' : -1;
}
static_assert(!mailDue(20261023,71999,0,0), "not before 20:00");
static_assert(mailDue(20261023,72000,0,0), "20:00 first report");
static_assert(!mailDue(20261023,86399,20261023,0), "no repeat today");
static_assert(!mailDue(20261023,72000,0,20261023), "pending report not duplicated");
static_assert(mailDue(20261024,72000,20261023,0), "next day");
static_assert(!mailOutageDue(119,0,false) && mailOutageDue(120,0,false), "debounce");
static_assert(!mailOutageDue(1000,0,true), "one warning per incident");
static_assert(!mailRecovered(129,100) && mailRecovered(130,100), "stable recovery");
static_assert(mailCanAttempt(2,900,900) && !mailCanAttempt(3,900,900), "retry limit");
static_assert(!mailCanAttempt(1,899,900), "retry backoff");
static_assert(mailReplyCode("250 ok")==250 && mailReplyCode("250-AUTH")==250, "SMTP replies");
static_assert(mailReplyCode("535 denied")==535 && mailReplyCode("bad reply")==-1, "SMTP rejects");
#endif
