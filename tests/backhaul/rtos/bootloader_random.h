#pragma once
extern bool fixtureEntropyEnabled;
inline void bootloader_random_enable() { fixtureEntropyEnabled=true; }
inline void bootloader_random_disable() { fixtureEntropyEnabled=false; }
