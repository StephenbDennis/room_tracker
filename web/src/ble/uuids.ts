/* GATT UUIDs. Must match ble_gatt.c. */

const base = (last: string) => `f0d2a450-1e4b-4c3a-9d1f-0000000000${last}`;

export const SVC_UUID = base('00');
export const CHR_CONFIG_WRITE = base('01');
export const CHR_CONFIG_READ = base('02');
export const CHR_TRACKS = base('03');
export const CHR_ZONE_STATE = base('04');
export const CHR_STATUS = base('05');
export const CHR_COMMAND = base('06');

export const enum Cmd {
  Identify = 0x01,
  Reboot = 0x02,
  FactoryReset = 0x03,
  GpioTest = 0x04,
  Save = 0x05,
}
