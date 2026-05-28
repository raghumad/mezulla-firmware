#pragma once

namespace MezullaScreenDump {

// Dump the OLED frame buffer (1024 bytes) to serial as hex.
// Call from serial console or after claim to verify screen state.
void dumpToSerial();

// Returns true if the buffer contains a QR code pattern
// (checks for the three finder patterns in corners).
bool bufferContainsQR();

// Returns true if the buffer is mostly blank (< 5% pixels set).
bool bufferIsBlank();

}
