#ifndef STRATUM_API_H
#define STRATUM_API_H

#include "cJSON.h"
#include <stdint.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <vector>

#define MAX_MERKLE_BRANCHES 32
#define MAX_EXTRANONCE2_SIZE 16 // bytes; calculateMiningData writes up to 2x that many hex digits
#define HASH_SIZE 32
#define COINBASE_SIZE 100
#define COINBASE2_SIZE 128

#define BUFFER_JSON_DOC 4096
#define BUFFER 1024
// Longest mining.notify accepted (its JSON document is sized from the line):
// pools with many payout outputs send coinbases of several KB.
#define MAX_NOTIFY_LEN 32768

typedef struct {
    String sub_details;
    String extranonce1;
    String extranonce2;
    int extranonce2_size;
    char wName[80];  // same size as Settings.BtcWallet
    char wPass[80];  // same size as Settings.PoolPassword (20 overflowed with a long password)
} mining_subscribe;

typedef struct {
    String job_id;
    String prev_block_hash;
    String coinb1;
    String coinb2;
    String nbits;
    std::vector<String> merkle_branch; // own copy: a JsonArray pointed into the shared JSON document
    String version;
    uint32_t target;
    String ntime;
    bool clean_jobs;
} mining_job;

typedef enum {
    STRATUM_SUCCESS,
    STRATUM_UNKNOWN,
    STRATUM_PARSE_ERROR,
    MINING_NOTIFY,
    MINING_SET_DIFFICULTY
} stratum_method;

unsigned long getNextId(unsigned long id);
bool verifyPayload (String* line);
bool checkError(const JsonDocument& doc);

//Method Mining.subscribe
mining_subscribe init_mining_subscribe(void);
bool tx_mining_subscribe(WiFiClient& client, mining_subscribe& mSubscribe);
bool parse_mining_subscribe(String line, mining_subscribe& mSubscribe);

//Method Mining.authorise
bool tx_mining_auth(WiFiClient& client, const char * user, const char * pass, unsigned long &auth_id);
stratum_method parse_mining_method(String line);
bool parse_mining_notify(String line, mining_job& mJob);

//Method Mining.submit
bool tx_mining_submit(WiFiClient& client, mining_subscribe mWorker, mining_job mJob, unsigned long nonce, unsigned long &submit_id);

//Difficulty Methods 
bool tx_suggest_difficulty(WiFiClient& client, double difficulty);
bool parse_mining_set_difficulty(String line, double& difficulty);

unsigned long parse_extract_id(const String &line);
bool parse_extract_result(const String &line);

#endif // STRATUM_API_H