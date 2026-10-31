import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:bluez/bluez.dart';
import 'package:flutter/foundation.dart';
import 'package:universal_ble/universal_ble.dart';

const deviceName = 'AudioLink';
const serviceUuid = '6b1e0000-7a3c-4f5e-9d2a-3c8f1b6e4a20';
const gainsUuid = '6b1e0001-7a3c-4f5e-9d2a-3c8f1b6e4a20';
const ledMaxUuid = '6b1e0002-7a3c-4f5e-9d2a-3c8f1b6e4a20';
const ledThemeUuid = '6b1e0003-7a3c-4f5e-9d2a-3c8f1b6e4a20';

const bandLabels = ['60 Hz', '250 Hz', '1 kHz', '4 kHz', '12 kHz'];
const gainMaxDb = 6;

List<int> decodeGains(Uint8List bytes) => Int8List.sublistView(bytes).toList();

Uint8List encodeGains(List<int> gains) => Uint8List.sublistView(Int8List.fromList(gains));

Future<BleDevice> _scan(Duration timeout) async {
  final found = Completer<BleDevice>();
  final sub = UniversalBle.scanStream.listen((device) {
    if (device.name == deviceName && !found.isCompleted) {
      found.complete(device);
    }
  });

  try {
    await UniversalBle.startScan(scanFilter: ScanFilter(withNamePrefix: [deviceName]));
    return await found.future.timeout(timeout);
  } finally {
    await sub.cancel();
    await UniversalBle.stopScan();
  }
}

// The board advertises once per scan, and universal_ble on Linux can miss that single report.
// BlueZ connects to a paired device by address without needing an advertisement.
Future<BleDevice?> _findPairedOnLinux() async {
  if (!Platform.isLinux) {
    return null;
  }

  final client = BlueZClient();
  try {
    await client.connect();
    for (final device in client.devices) {
      if (device.paired && device.name == deviceName) {
        return BleDevice(deviceId: device.address, name: device.name);
      }
    }
    return null;
  } finally {
    await client.close();
  }
}

Future<BleDevice> _find() async {
  // A phone streaming LE Audio is already connected, and the board stops advertising then
  final system = await UniversalBle.getSystemDevices(withServices: [serviceUuid]);
  for (final device in system) {
    if (device.name == deviceName) {
      debugPrint('Found connected ${device.deviceId}');
      return device;
    }
  }

  final paired = await _findPairedOnLinux();
  if (paired != null) {
    debugPrint('Found paired ${paired.deviceId}');
    return paired;
  }

  debugPrint('Scanning');
  final scanned = await _scan(const Duration(seconds: 10));
  debugPrint('Found by scan ${scanned.deviceId}');
  return scanned;
}

class AudioLink {
  AudioLink._(this.device, this._gains, this._ledMax, this._ledTheme);

  final BleDevice device;
  final BleCharacteristic _gains;
  final BleCharacteristic _ledMax;
  final BleCharacteristic? _ledTheme;

  static Future<AudioLink> connect() async {
    await UniversalBle.requestPermissions();
    final device = await _find();
    debugPrint('Connecting');
    await device.connect();

    // Android caches services per bonded device, so a phone paired before the EQ service existed
    // would not see it
    debugPrint('Discovering services');
    await device.discoverServices(
      platformConfig: DiscoverServicesPlatformConfig(
        android: AndroidDiscoverServicesOptions(clearGattCache: true),
      ),
    );

    return AudioLink._(
      device,
      await device.getCharacteristic(gainsUuid, service: serviceUuid),
      await device.getCharacteristic(ledMaxUuid, service: serviceUuid),
      await _optionalCharacteristic(device, ledThemeUuid),
    );
  }

  // Only the Matter firmware has LED themes
  static Future<BleCharacteristic?> _optionalCharacteristic(BleDevice device, String uuid) async {
    try {
      return await device.getCharacteristic(uuid, service: serviceUuid);
    } on UniversalBleException catch (e) {
      if (e.code == UniversalBleErrorCode.characteristicNotFound) {
        return null;
      }
      rethrow;
    }
  }

  Stream<bool> get connectionStream => device.connectionStream;

  Future<List<int>> readGains() async {
    debugPrint('Reading gains');
    final gains = decodeGains(await _gains.read(timeout: const Duration(seconds: 10)));
    debugPrint('Gains $gains');
    return gains;
  }

  Future<void> writeGains(List<int> gains) => _gains.write(encodeGains(gains));

  Future<int> readLedMax() async {
    final percent = (await _ledMax.read(timeout: const Duration(seconds: 10)))[0];
    debugPrint('LED max $percent %');
    return percent;
  }

  Future<void> writeLedMax(int percent) => _ledMax.write([percent]);

  /// Null when the firmware has no LED themes
  Future<int?> readLedTheme() async {
    final characteristic = _ledTheme;
    if (characteristic == null) {
      return null;
    }

    final theme = (await characteristic.read(timeout: const Duration(seconds: 10)))[0];
    debugPrint('LED theme $theme');
    return theme;
  }

  Future<void> writeLedTheme(int theme) => _ledTheme!.write([theme]);

  Future<void> disconnect() => device.disconnect();
}
