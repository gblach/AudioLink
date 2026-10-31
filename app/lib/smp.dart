import 'dart:async';
import 'dart:collection';
import 'dart:io';
import 'dart:math';
import 'dart:typed_data';

import 'package:cbor/simple.dart';
import 'package:flutter/foundation.dart';
import 'package:universal_ble/universal_ble.dart';

const _serviceUuid = '8d53dc1d-1db7-4cd3-868b-8a527460aa84';
const _characteristicUuid = 'da2e7828-fbce-4e01-ae9e-261174997c48';

// CONFIG_BT_L2CAP_TX_MTU of the firmware
const _mtu = 498;
const _headerSize = 8;
// Room for the CBOR map keys and integers around the data of an upload request
const _uploadOverhead = 64;
const _timeout = Duration(seconds: 10);

const _opRead = 0;
const _opWrite = 2;
const _version2 = 1 << 3;

const _groupOs = 0;
const _groupImage = 1;
const _idOsReset = 5;
const _idOsParams = 6;
const _idImageState = 0;
const _idImageUpload = 1;

class SmpException implements Exception {
  SmpException(this.message);

  final String message;

  @override
  String toString() => 'SMP error: $message';
}

/// MCUmgr Simple Management Protocol over Bluetooth, the firmware update service of the board
class Smp {
  Smp._(this._characteristic, this._writeSize);

  final BleCharacteristic _characteristic;
  final int _writeSize;
  int _bufferSize = 0;
  int _window = 1;
  final _received = BytesBuilder();
  StreamSubscription<Uint8List>? _sub;
  final _responses = <int, Completer<Uint8List>>{};
  Future<void> _writes = Future.value();
  int _seq = 0;

  static Future<Smp> open(BleDevice device) async {
    final characteristic = await device.getCharacteristic(
      _characteristicUuid,
      service: _serviceUuid,
    );
    final mtu = await device.requestMtu(_mtu);
    // universal_ble returns the ATT MTU on Android, but the payload size (MTU - 3) on Linux
    final writeSize = Platform.isLinux ? mtu : mtu - 3;
    debugPrint('SMP write size $writeSize');

    final smp = Smp._(characteristic, writeSize);
    smp._sub = characteristic.onValueReceived.listen(smp._onValue);
    await characteristic.notifications.subscribe();

    final params = await smp._request(_opRead, _groupOs, _idOsParams, {});
    smp._bufferSize = params['buf_size'] as int;
    // Upload requests in flight, the board needs one more buffer for the request being
    // received and one for the response
    smp._window = max(1, (params['buf_count'] as int) - 2);
    debugPrint('SMP buffer size ${smp._bufferSize}, window ${smp._window}');

    return smp;
  }

  // A response longer than the MTU arrives in several notifications
  void _onValue(Uint8List value) {
    _received.add(value);
    var bytes = _received.takeBytes();

    while (bytes.length >= _headerSize) {
      final length = _headerSize + (bytes[2] << 8 | bytes[3]);
      if (bytes.length < length) {
        break;
      }

      _responses.remove(bytes[6])?.complete(Uint8List.sublistView(bytes, 0, length));
      bytes = Uint8List.sublistView(bytes, length);
    }

    _received.add(bytes);
  }

  Future<void> _write(Uint8List packet) async {
    for (var off = 0; off < packet.length; off += _writeSize) {
      final end = min(off + _writeSize, packet.length);
      await _characteristic.write(Uint8List.sublistView(packet, off, end), withResponse: false);
    }
  }

  // Requests may overlap, so their writes are chained to keep the packets from interleaving
  Future<Map<Object?, Object?>> _request(
    int op,
    int group,
    int id,
    Map<String, Object> body,
  ) async {
    final data = cbor.encode(body);
    final seq = _seq++ & 0xff;
    final packet = Uint8List.fromList([
      _version2 | op,
      0,
      data.length >> 8,
      data.length & 0xff,
      group >> 8,
      group & 0xff,
      seq,
      id,
      ...data,
    ]);

    final response = Completer<Uint8List>();
    _responses[seq] = response;

    final Uint8List bytes;
    try {
      final written = _writes.then((_) => _write(packet));
      _writes = written.catchError((_) {});
      await written;
      bytes = await response.future.timeout(_timeout);
    } finally {
      _responses.remove(seq);
    }

    final map = cbor.decode(bytes.sublist(_headerSize)) as Map<Object?, Object?>;
    final rc = map['rc'] as int? ?? 0;
    final err = map['err'] as Map<Object?, Object?>?;
    if (rc != 0) {
      throw SmpException('group $group command $id failed with rc $rc');
    }
    if (err != null) {
      throw SmpException('group ${err['group']} command $id failed with rc ${err['rc']}');
    }

    return map;
  }

  Future<void> upload(int image, Uint8List data, void Function(int offset) onProgress) async {
    final chunkSize = _bufferSize - _headerSize - _uploadOverhead;
    final inFlight = Queue<(int, Future<Map<Object?, Object?>>)>();
    var next = 0;
    var off = 0;

    try {
      while (off < data.length) {
        while (inFlight.length < _window && next < data.length) {
          final end = min(next + chunkSize, data.length);
          final body = <String, Object>{
            'off': next,
            'data': Uint8List.sublistView(data, next, end),
          };
          if (next == 0) {
            body['image'] = image;
            body['len'] = data.length;
          }

          inFlight.add((end, _request(_opWrite, _groupImage, _idImageUpload, body)));
          next = end;
        }

        final (end, response) = inFlight.removeFirst();
        off = (await response)['off'] as int;
        onProgress(off);

        // The board skipped this chunk, so the ones sent after it are rejected too
        if (off != end) {
          await Future.wait(inFlight.map((request) => request.$2.catchError((_) => {})));
          inFlight.clear();
          next = off;
        }
      }
    } finally {
      for (final (_, response) in inFlight) {
        response.ignore();
      }
    }
  }

  /// Version of the running application image, for example "1.0.0"
  Future<String> runningVersion() async {
    final state = await _request(_opRead, _groupImage, _idImageState, {});
    final slots = (state['images'] as List<Object?>).cast<Map<Object?, Object?>>();
    final active = slots.firstWhere((slot) => (slot['image'] ?? 0) == 0 && slot['active'] == true);
    return active['version'] as String;
  }

  /// Marks the uploaded images in the secondary slots for installation on the next boot
  Future<void> confirmUploaded(Set<int> images) async {
    final state = await _request(_opRead, _groupImage, _idImageState, {});
    final slots = (state['images'] as List<Object?>).cast<Map<Object?, Object?>>();
    final confirmed = <int>{};

    for (final slot in slots) {
      final image = slot['image'] as int? ?? 0;
      if (slot['slot'] != 1 || !images.contains(image)) {
        continue;
      }

      final hash = Uint8List.fromList((slot['hash'] as List<Object?>).cast<int>());
      // MCUboot runs in overwrite mode, there is no rollback, so test and confirm are the same
      await _request(_opWrite, _groupImage, _idImageState, {'hash': hash, 'confirm': true});
      confirmed.add(image);
    }

    if (!setEquals(confirmed, images)) {
      throw SmpException('uploaded images ${images.difference(confirmed)} not found');
    }
  }

  Future<void> reset() => _request(_opWrite, _groupOs, _idOsReset, {});

  Future<void> close() async {
    await _sub?.cancel();
    try {
      await _characteristic.notifications.unsubscribe();
    } catch (_) {}
  }
}
