#include "mailPolicy.h"
#include "poolRetryPolicy.h"
static_assert(!mailDue(20261025,71999,20261024,0),"before report time");
static_assert(mailDue(20261025,72000,20261024,0),"at report time");
static_assert(!mailCanAttempt(3,10000,0),"exhausted report stays exhausted");
static_assert(mailDue(20261025,72000,20261024,20261024),"new day gets a fresh report");
