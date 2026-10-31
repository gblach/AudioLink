import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';

import 'package:audiolink/audiolink.dart';

void main() {
  test('gains are signed bytes, band 1 first', () {
    expect(encodeGains([4, 0, 0, 0, -6]), Uint8List.fromList([0x04, 0, 0, 0, 0xFA]));
    expect(decodeGains(Uint8List.fromList([0xFF, 0x02, 0, 0, 0x06])), [-1, 2, 0, 0, 6]);
  });
}
