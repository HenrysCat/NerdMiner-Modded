#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include "stratum.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "lwip/sockets.h"
#include "utils.h"
#include "version.h"



StaticJsonDocument<BUFFER_JSON_DOC> doc;
unsigned long id = 1;

//Get next JSON RPC Id
unsigned long getNextId(unsigned long id) {
    if (id == ULONG_MAX) {
      id = 1;
      return id;
    }
    return ++id;
}

// A value put between quotes in a JSON request: a " or \ in a worker name or a
// password ended the string early and the pool got invalid JSON.
static String jsonEscape(const char *s)
{
    String out;
    out.reserve(strlen(s) + 8);
    for (; *s; ++s)
    {
        const char c = *s;
        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += c;
        }
        else if ((uint8_t)c < 0x20)
        {
            char u[7];
            snprintf(u, sizeof(u), "\\u%04x", (unsigned)(uint8_t)c);
            out += u;
        }
        else
            out += c;
    }
    return out;
}

static bool isHex(const String &s)
{
    for (size_t i = 0; i < s.length(); ++i)
        if (!isxdigit((unsigned char)s[i]))
            return false;
    return true;
}

// Only the envelope of a message (method, id, error, result) goes into the
// shared document: a mining.notify bigger than BUFFER_JSON_DOC made the full
// parse fail with NoMemory, the message was taken for an error and dropped
// without a word, and the miner kept hashing the old job.
static DeserializationError parseEnvelope(const String &line)
{
    static StaticJsonDocument<128> filter;
    if (filter.isNull())
    {
        filter["method"] = true;
        filter["id"] = true;
        filter["error"] = true;
        filter["result"] = true;
    }
    return deserializeJson(doc, line, DeserializationOption::Filter(filter));
}

//Verify Payload doesn't has zero lenght
bool verifyPayload (String* line){
  if(line->length() == 0) return false;
  line->trim();
  if(line->isEmpty()) return false;
  return true;
  
}

// Pools send the error either as [code, "message", data] or as {"code":..,"message":..}.
// A missing field must not reach %s as NULL: that was a LoadProhibited reboot (issue #4).
bool checkError(const JsonDocument& doc) {

  if (!doc.containsKey("error")) return false;

  JsonVariantConst err = doc["error"];
  if (err.isNull()) return false;

  int code = 0;
  const char* reason = nullptr;
  if (err.is<JsonArrayConst>()) {
    if (err.size() == 0) return false;
    code = err[0] | 0;
    reason = err[1];
  } else if (err.is<JsonObjectConst>()) {
    if (err.size() == 0) return false;
    code = err["code"] | 0;
    reason = err["message"];
  } else if (err.is<const char*>()) {
    reason = err.as<const char*>();
  } else {
    return false; // false / 0: not an error
  }

  Serial.printf("ERROR: %d | reason: %s \n", code, reason ? reason : "?");

  return true;
}


// STEP 1: Pool server connection (SUBSCRIBE)
    // Docs: 
    // - https://cs.braiins.com/stratum-v1/docs
    // - https://github.com/aeternity/protocol/blob/master/STRATUM.md#mining-subscribe
bool tx_mining_subscribe(WiFiClient& client, mining_subscribe& mSubscribe)
{
    char payload[BUFFER] = {0};
    
    // Subscribe
    id = 1; //Initialize id messages
    #ifndef HAN
    snprintf(payload, sizeof(payload), "{\"id\": %lu, \"method\": \"mining.subscribe\", \"params\": [\"NerdMinerV2/%s\"]}\n", id, CURRENT_VERSION);
    #else
    snprintf(payload, sizeof(payload), "{\"id\": %lu, \"method\": \"mining.subscribe\", \"params\": [\"HAN_SOLOminer/%s\"]}\n", id, CURRENT_VERSION);
    #endif
    
    Serial.printf("[WORKER] ==> Mining subscribe\n");
    Serial.print("  Sending  : "); Serial.println(payload);
    client.print(payload);
    
    vTaskDelay(200 / portTICK_PERIOD_MS); //Small delay
    
    String line = client.readStringUntil('\n');
    if(!parse_mining_subscribe(line, mSubscribe)) return false;

  
    Serial.print("    sub_details: "); Serial.println(mSubscribe.sub_details);
    Serial.print("    extranonce1: "); Serial.println(mSubscribe.extranonce1);
    Serial.print("    extranonce2_size: "); Serial.println(mSubscribe.extranonce2_size);

    if((mSubscribe.extranonce1.length() == 0) ) { 
        Serial.printf("[WORKER] >>>>>>>>> Work aborted\n"); 
        Serial.printf("extranonce1 length: %u \n", mSubscribe.extranonce1.length());
        doc.clear();
        doc.garbageCollect();
        return false; 
    }
    return true;
}

bool parse_mining_subscribe(String line, mining_subscribe& mSubscribe)
{
    if(!verifyPayload(&line)) return false;
    Serial.print("  Receiving: "); Serial.println(line);
   
    DeserializationError error = deserializeJson(doc, line);

    if (error || checkError(doc)) return false;
    if (!doc.containsKey("result")) return false;

    mSubscribe.sub_details = String((const char*) doc["result"][0][0][1]);
    mSubscribe.extranonce1 = String((const char*) doc["result"][1]);
    mSubscribe.extranonce2_size = doc["result"][2] | 0;

    // Both end up in the coinbase: a bad value meant every share rejected
    // (extranonce2_size used to fall back to 4 without a word).
    if (mSubscribe.extranonce1.isEmpty() || (mSubscribe.extranonce1.length() & 1) || !isHex(mSubscribe.extranonce1))
    {
        Serial.printf("    subscribe refused: bad extranonce1 \"%s\"\n", mSubscribe.extranonce1.c_str());
        return false;
    }
    if (mSubscribe.extranonce2_size < 1 || mSubscribe.extranonce2_size > MAX_EXTRANONCE2_SIZE)
    {
        Serial.printf("    subscribe refused: extranonce2_size %d (1 to %d)\n", mSubscribe.extranonce2_size, MAX_EXTRANONCE2_SIZE);
        return false;
    }

    return true;
}

mining_subscribe init_mining_subscribe(void)
{
    mining_subscribe new_mSub;

    new_mSub.extranonce1 = "";
    new_mSub.extranonce2 = "";
    new_mSub.extranonce2_size = 0;
    new_mSub.sub_details = "";


    return new_mSub;
}

// STEP 2: Pool server auth (authorize)
bool tx_mining_auth(WiFiClient& client, const char * user, const char * pass, unsigned long &auth_id)
{
    char payload[BUFFER] = {0};

    // Authorize
    id = getNextId(id);
    auth_id = id; // the pool's answer to this id says whether it took the wallet
    snprintf(payload, sizeof(payload), "{\"params\": [\"%s\", \"%s\"], \"id\": %lu, \"method\": \"mining.authorize\"}\n",
      jsonEscape(user).c_str(), jsonEscape(pass).c_str(), id);
    
    Serial.printf("[WORKER] ==> Autorize work\n");
    // Not the payload itself: it holds the pool password
    Serial.printf("  Sending  : mining.authorize \"%s\", password %s\n", user, (pass && *pass) ? "(set)" : "(empty)");
    client.print(payload);

    vTaskDelay(200 / portTICK_PERIOD_MS); //Small delay

    // The answer is parsed with the other messages in the stratum loop
    // (runStratumWorker, auth_id).

    return true;
}


stratum_method parse_mining_method(String line)
{
    if(!verifyPayload(&line)) return STRATUM_PARSE_ERROR;
    Serial.print("  Receiving: "); Serial.println(line);
    
    DeserializationError error = parseEnvelope(line);
    if (error) {
      Serial.printf("    not parsed (%u bytes): %s\n", line.length(), error.c_str());
      return STRATUM_PARSE_ERROR;
    }
    if (checkError(doc)) return STRATUM_PARSE_ERROR;

    if (!doc.containsKey("method")) {
      // A reply. checkError() above already caught a real error, so what is
      // left ("error": null, missing, or false/0 as some pools send) is a
      // success; "error": false used to fall into STRATUM_UNKNOWN and the
      // share was never counted.
      return STRATUM_SUCCESS;
    }
    stratum_method result = STRATUM_UNKNOWN;

    if (strcmp("mining.notify", (const char*) doc["method"]) == 0) {
        result = MINING_NOTIFY;
    } else if (strcmp("mining.set_difficulty", (const char*) doc["method"]) == 0) {
        result = MINING_SET_DIFFICULTY;
    }

    return result;
}

bool parse_mining_notify(String line, mining_job& mJob)
{
    Serial.println("    Parsing Method [MINING NOTIFY]");
    if(!verifyPayload(&line)) return false;

    if (line.length() > MAX_NOTIFY_LEN) {
      Serial.printf("    mining.notify refused: %u bytes (max %u)\n", line.length(), (unsigned)MAX_NOTIFY_LEN);
      return false;
    }
    // A document of its own, sized from the message: the strings are copied
    // in (about the length of the line) plus the slots of the params array
    // and the merkle branch. The shared 4 KB one was too small for big
    // coinbases.
    DynamicJsonDocument job(line.length() + JSON_OBJECT_SIZE(4) + JSON_ARRAY_SIZE(16) +
                            JSON_ARRAY_SIZE(MAX_MERKLE_BRANCHES) + 256);
    if (job.capacity() == 0) {
      Serial.printf("    mining.notify not parsed (%u bytes): out of memory\n", line.length());
      return false;
    }
    DeserializationError error = deserializeJson(job, line);

    if (error) {
      Serial.printf("    mining.notify not parsed (%u bytes): %s\n", line.length(), error.c_str());
      return false;
    }
    if (!job.containsKey("params")) return false;
    if (checkError(job)) {
      Serial.printf("[WORKER] >>>>>>>>> Work aborted\n");
      return false;
    }

    JsonArrayConst params = job["params"];
    JsonArrayConst branch = params[4];
    mJob.job_id = String((const char*) params[0]);
    mJob.prev_block_hash = String((const char*) params[1]);
    mJob.coinb1 = String((const char*) params[2]);
    mJob.coinb2 = String((const char*) params[3]);
    mJob.version = String((const char*) params[5]);
    mJob.nbits = String((const char*) params[6]);
    mJob.ntime = String((const char*) params[7]);
    mJob.clean_jobs = params[8] | false;
    mJob.merkle_branch.clear();

    // Sizes the header code relies on (calculateMiningData copies them into
    // fixed buffers): a malformed job is refused instead of overflowing them.
    {
      const char *bad = nullptr;
      const int nbitsExp = mJob.nbits.length() == 8 && isHex(mJob.nbits)
                               ? (int)strtol(mJob.nbits.substring(0, 2).c_str(), nullptr, 16) : -1;
      if (mJob.job_id.isEmpty()) bad = "job_id";
      else if (mJob.prev_block_hash.length() != 64 || !isHex(mJob.prev_block_hash)) bad = "prevhash";
      else if (mJob.version.length() != 8 || !isHex(mJob.version)) bad = "version";
      else if (nbitsExp < 5 || nbitsExp > 0x20) bad = "nbits";
      else if (mJob.ntime.length() != 8 || !isHex(mJob.ntime)) bad = "ntime";
      else if ((mJob.coinb1.length() | mJob.coinb2.length()) & 1 || !isHex(mJob.coinb1) || !isHex(mJob.coinb2)) bad = "coinbase";
      else if (branch.isNull() || branch.size() > MAX_MERKLE_BRANCHES) bad = "merkle_branch";
      else
        for (JsonVariantConst m : branch)
        {
          const char *h = m.as<const char *>();
          if (!h || strlen(h) != 64 || !isHex(String(h))) { bad = "merkle_branch"; break; }
        }
      if (bad)
      {
        Serial.printf("    mining.notify refused: bad %s\n", bad);
        return false;
      }
    }
    // Copied out: the document goes away with this function.
    mJob.merkle_branch.reserve(branch.size());
    for (JsonVariantConst m : branch)
      mJob.merkle_branch.push_back(String(m.as<const char *>()));

    #ifdef DEBUG_MINING
    Serial.print("    job_id: "); Serial.println(mJob.job_id);
    Serial.print("    prevhash: "); Serial.println(mJob.prev_block_hash);
    Serial.print("    coinb1: "); Serial.println(mJob.coinb1);
    Serial.print("    coinb2: "); Serial.println(mJob.coinb2);
    Serial.print("    merkle_branch size: "); Serial.println(mJob.merkle_branch.size());
    Serial.print("    version: "); Serial.println(mJob.version);
    Serial.print("    nbits: "); Serial.println(mJob.nbits);
    Serial.print("    ntime: "); Serial.println(mJob.ntime);
    Serial.print("    clean_jobs: "); Serial.println(mJob.clean_jobs);
    #endif
    return true;
}


bool tx_mining_submit(WiFiClient& client, mining_subscribe mWorker, mining_job mJob, unsigned long nonce, unsigned long &submit_id)
{
    char payload[BUFFER] = {0};
    char nonceHex[9];

    // Nonce must always be 8 hex digits: String(nonce, HEX) drops leading zeros,
    // and pools reject the short form with "No nonce" (~1 in 16 shares).
    snprintf(nonceHex, sizeof(nonceHex), "%08lx", nonce);

    // Submit
    id = getNextId(id);
    submit_id = id;
    snprintf(payload, sizeof(payload), "{\"id\":%lu,\"method\":\"mining.submit\",\"params\":[\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"]}\n",
        id,
        jsonEscape(mWorker.wName).c_str(),
        jsonEscape(mJob.job_id.c_str()).c_str(),
        mWorker.extranonce2.c_str(),
        mJob.ntime.c_str(),
        nonceHex
        );
    Serial.print("  Sending  : "); Serial.print(payload);
    client.print(payload);
    //Serial.print("  Receiving: "); Serial.println(client.readStringUntil('\n'));

    return true;
}

bool parse_mining_set_difficulty(String line, double& difficulty)
{
    Serial.println("    Parsing Method [SET DIFFICULTY]");
    if(!verifyPayload(&line)) return false;
   
    DeserializationError error = deserializeJson(doc, line);

    if (error) return false;
    if (!doc.containsKey("params")) return false;

    const double d = doc["params"][0] | 0.0;
    Serial.print("    difficulty: "); Serial.println(d, 12);
    if (!(d > 0) || isinf(d))
    {
        Serial.println("    difficulty ignored (not a positive number)");
        return false; // 0 or less would make every hash a share
    }
    difficulty = d;

    return true;
}

bool tx_suggest_difficulty(WiFiClient& client, double difficulty)
{
    char payload[BUFFER] = {0};

    id = getNextId(id);
    snprintf(payload, sizeof(payload), "{\"id\":%lu,\"method\":\"mining.suggest_difficulty\",\"params\":[%.10g]}\n", id, difficulty);
    
    Serial.print("  Sending  : "); Serial.print(payload);
    return client.print(payload);

}


unsigned long parse_extract_id(const String &line)
{
    DeserializationError error = parseEnvelope(line);
    if (error)
        return 0;
    
    if (!doc.containsKey("id"))
        return 0;

    unsigned long id = doc["id"];

    return id;
}

// A reply's "result": true when the pool accepted the submitted share.
bool parse_extract_result(const String &line)
{
    if (parseEnvelope(line))
        return false;
    return doc["result"] | false;
}