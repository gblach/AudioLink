import 'dart:convert';

import 'package:archive/archive.dart';
import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:universal_ble/universal_ble.dart';

import 'smp.dart';

const firmwareAsset = 'assets/dfu_application.zip';

class _Image {
  _Image(this.index, this.data);

  final int index;
  final Uint8List data;
}

Future<List<_Image>> _loadImages() async {
  final zip = await rootBundle.load(firmwareAsset);
  final archive = ZipDecoder().decodeBytes(Uint8List.sublistView(zip));
  final manifest = jsonDecode(utf8.decode(archive.findFile('manifest.json')!.content));

  return [
    for (final file in manifest['files'] as List<Object?>)
      _Image(
        int.parse((file as Map<String, Object?>)['image_index'] as String),
        archive.findFile(file['file'] as String)!.content,
      ),
  ];
}

// The version in the manifest is the one of the bootloader, the image header has the real one
List<int> _headerVersion(Uint8List image) {
  final header = ByteData.sublistView(image);
  return [header.getUint8(20), header.getUint8(21), header.getUint16(22, Endian.little)];
}

// MCUmgr adds the build number as a fourth part when it is not zero, it is not compared
List<int> _parseVersion(String version) => version.split('.').take(3).map(int.parse).toList();

bool _isNewer(List<int> a, List<int> b) {
  for (var i = 0; i < 3; i++) {
    if (a[i] != b[i]) {
      return a[i] > b[i];
    }
  }
  return false;
}

class FirmwareStatus {
  const FirmwareStatus(this.running, this.bundled);

  /// Version of the application image on the board, for example "1.0.0"
  final String running;

  /// Version of the application image bundled with the app
  final String bundled;

  bool get updateAvailable => _isNewer(_parseVersion(bundled), _parseVersion(running));
}

Future<FirmwareStatus> firmwareStatus(BleDevice device) async {
  final images = await _loadImages();
  final bundled = _headerVersion(images.firstWhere((image) => image.index == 0).data).join('.');
  final smp = await Smp.open(device);

  try {
    final running = await smp.runningVersion();
    debugPrint('Firmware running $running, bundled $bundled');
    return FirmwareStatus(running, bundled);
  } finally {
    await smp.close();
  }
}

enum UpdateStep { application, controller, confirm, restart }

/// Uploads the firmware bundled with the app to both cores and restarts the board into it.
/// [onProgress] gets the current step and the fraction of all images uploaded so far.
Future<void> updateFirmware(
  BleDevice device,
  void Function(UpdateStep step, double progress) onProgress,
) async {
  final images = await _loadImages();
  final total = images.fold(0, (sum, image) => sum + image.data.length);
  final smp = await Smp.open(device);

  try {
    var done = 0;
    for (final image in images) {
      final step = image.index == 0 ? UpdateStep.application : UpdateStep.controller;
      onProgress(step, done / total);
      await smp.upload(
        image.index,
        image.data,
        (offset) => onProgress(step, (done + offset) / total),
      );
      done += image.data.length;
    }

    onProgress(UpdateStep.confirm, 1);
    await smp.confirmUploaded({for (final image in images) image.index});
    onProgress(UpdateStep.restart, 1);
    await smp.reset();
  } finally {
    await smp.close();
  }
}
