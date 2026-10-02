#ifndef CURRENCY_H
#define CURRENCY_H

#include <Arduino.h>
#include <string.h>

// Fiat currencies the BTC price can be shown in. "code" is the lowercase
// vs_currency id used by the CoinGecko API (and stored in Settings.Currency);
// "prefix" is ASCII-only so every display font can render it; "symbol" is the
// real currency sign for fonts that carry the glyph.
struct CurrencyInfo
{
  const char *code;
  const char *label;
  const char *prefix; // ASCII-only
  const char *symbol; // UTF-8, for displays whose font has the glyph (Pulse)
};

static const CurrencyInfo kCurrencies[] = {
    {"usd", "USD - US Dollar", "$", "$"},
    {"gbp", "GBP - British Pound", "GBP ", "\xC2\xA3"},
    {"eur", "EUR - Euro", "EUR ", "\xE2\x82\xAC"},
    {"cad", "CAD - Canadian Dollar", "C$", "C$"},
    {"aud", "AUD - Australian Dollar", "A$", "A$"},
    {"nzd", "NZD - New Zealand Dollar", "NZ$", "NZ$"},
    {"chf", "CHF - Swiss Franc", "CHF ", "CHF "},
    {"jpy", "JPY - Japanese Yen", "JPY ", "\xC2\xA5"},
    {"cny", "CNY - Chinese Yuan", "CNY ", "\xC2\xA5"},
    {"inr", "INR - Indian Rupee", "INR ", "\xE2\x82\xB9"},
    {"krw", "KRW - South Korean Won", "KRW ", "\xE2\x82\xA9"},
    {"sgd", "SGD - Singapore Dollar", "S$", "S$"},
    {"hkd", "HKD - Hong Kong Dollar", "HK$", "HK$"},
    {"brl", "BRL - Brazilian Real", "R$", "R$"},
    {"mxn", "MXN - Mexican Peso", "MX$", "MX$"},
    {"sek", "SEK - Swedish Krona", "kr ", "kr "},
    {"nok", "NOK - Norwegian Krone", "kr ", "kr "},
    {"dkk", "DKK - Danish Krone", "kr ", "kr "},
    {"pln", "PLN - Polish Zloty", "PLN ", "PLN "},
    {"czk", "CZK - Czech Koruna", "CZK ", "CZK "},
    {"zar", "ZAR - South African Rand", "R", "R"},
    {"try", "TRY - Turkish Lira", "TRY ", "\xE2\x82\xBA"},
};
static const int kCurrencyCount = sizeof(kCurrencies) / sizeof(kCurrencies[0]);

// Returns the matching entry, or USD when the code is unknown.
static inline const CurrencyInfo &currencyFor(const String &code)
{
  for (int i = 0; i < kCurrencyCount; i++)
    if (code.equalsIgnoreCase(kCurrencies[i].code))
      return kCurrencies[i];
  return kCurrencies[0];
}

#endif // CURRENCY_H
