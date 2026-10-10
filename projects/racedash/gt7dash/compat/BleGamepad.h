/* 原作有藍牙手把功能（已註解掉不用），只要能編譯。 */
#pragma once
class BleGamepadConfiguration { public: void setAutoReport(bool) {} void setButtonCount(int) {} };
class BleGamepad
{
public:
    BleGamepad(const char * = "", const char * = "", int = 100) {}
    void begin(BleGamepadConfiguration * = nullptr) {}
    bool isConnected() { return false; }
};
