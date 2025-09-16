#pragma once
struct PowerState {
  bool usb, charging, full;
  int  percent;
  float vbat;
};
