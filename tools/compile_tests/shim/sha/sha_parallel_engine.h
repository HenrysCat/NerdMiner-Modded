#pragma once
enum esp_sha_type { SHA1, SHA2_256, SHA2_512 };
bool esp_sha_try_lock_engine(esp_sha_type);
void esp_sha_unlock_engine(esp_sha_type);
