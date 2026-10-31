import 'dart:math';

import 'package:flutter/material.dart';

import 'audiolink.dart';
import 'theme.dart';

enum _BandType { lowShelf, peak, highShelf }

// Must match bands[] and the constants in firmware/src/eq.c
const _bands = [
  (_BandType.lowShelf, 60.0),
  (_BandType.peak, 250.0),
  (_BandType.peak, 1000.0),
  (_BandType.peak, 4000.0),
  (_BandType.highShelf, 12000.0),
];
const _sampleRate = 48000.0;
const _q = 0.707;

// Audio EQ Cookbook (R. Bristow-Johnson), b0 b1 b2 a0 a1 a2
List<double> _coefs(_BandType type, double freq, int gainDb) {
  final a = pow(10, gainDb / 40).toDouble();
  final w0 = 2 * pi * freq / _sampleRate;
  final cw = cos(w0);
  final alpha = sin(w0) / (2 * _q);
  final sa = 2 * sqrt(a) * alpha;

  return switch (type) {
    _BandType.lowShelf => [
      a * ((a + 1) - (a - 1) * cw + sa),
      2 * a * ((a - 1) - (a + 1) * cw),
      a * ((a + 1) - (a - 1) * cw - sa),
      (a + 1) + (a - 1) * cw + sa,
      -2 * ((a - 1) + (a + 1) * cw),
      (a + 1) + (a - 1) * cw - sa,
    ],
    _BandType.highShelf => [
      a * ((a + 1) + (a - 1) * cw + sa),
      -2 * a * ((a - 1) + (a + 1) * cw),
      a * ((a + 1) + (a - 1) * cw - sa),
      (a + 1) - (a - 1) * cw + sa,
      2 * ((a - 1) - (a + 1) * cw),
      (a + 1) - (a - 1) * cw - sa,
    ],
    _BandType.peak => [
      1 + alpha * a,
      -2 * cw,
      1 - alpha * a,
      1 + alpha / a,
      -2 * cw,
      1 - alpha / a,
    ],
  };
}

double _magnitudeDb(List<double> c, double freq) {
  final w = 2 * pi * freq / _sampleRate;
  final nr = c[0] + c[1] * cos(w) + c[2] * cos(2 * w);
  final ni = -(c[1] * sin(w) + c[2] * sin(2 * w));
  final dr = c[3] + c[4] * cos(w) + c[5] * cos(2 * w);
  final di = -(c[4] * sin(w) + c[5] * sin(2 * w));
  return 10 * log((nr * nr + ni * ni) / (dr * dr + di * di)) / ln10;
}

/// Frequency response of the board's equalizer, 20 Hz to 20 kHz
class EqCurve extends StatelessWidget {
  const EqCurve({super.key, required this.gains});

  final List<int> gains;

  @override
  Widget build(BuildContext context) {
    final c = context.colors;
    return AspectRatio(
      aspectRatio: 300 / 120,
      child: DecoratedBox(
        decoration: BoxDecoration(
          color: c.well,
          border: Border.all(color: c.line),
          borderRadius: BorderRadius.circular(12),
        ),
        child: ClipRRect(
          borderRadius: BorderRadius.circular(12),
          child: CustomPaint(painter: _CurvePainter(gains, c)),
        ),
      ),
    );
  }
}

class _CurvePainter extends CustomPainter {
  _CurvePainter(this.gains, this.c);

  final List<int> gains;
  final AppColors c;

  static const _rangeDb = 9.0;
  static const _points = 120;

  @override
  void paint(Canvas canvas, Size size) {
    double x(double freq) => log(freq / 20) / log(1000) * size.width;
    double y(double db) => size.height / 2 - db / _rangeDb * (size.height / 2 - 8);

    final grid = Paint()
      ..color = c.line
      ..strokeWidth = 1;
    for (final freq in [100.0, 1000.0, 10000.0]) {
      canvas.drawLine(Offset(x(freq), 0), Offset(x(freq), size.height), grid);
    }
    for (final db in [-6.0, 6.0]) {
      for (var dx = 0.0; dx < size.width; dx += 6) {
        canvas.drawLine(Offset(dx, y(db)), Offset(dx + 2, y(db)), grid);
      }
    }
    canvas.drawLine(
      Offset(0, y(0)),
      Offset(size.width, y(0)),
      Paint()
        ..color = c.muted.withValues(alpha: 0.5)
        ..strokeWidth = 1,
    );

    final coefs = [
      for (var i = 0; i < _bands.length; i++) _coefs(_bands[i].$1, _bands[i].$2, gains[i]),
    ];
    double response(double freq) => coefs.fold(0, (sum, c) => sum + _magnitudeDb(c, freq));

    final line = Path();
    for (var i = 0; i <= _points; i++) {
      final freq = 20 * pow(1000, i / _points).toDouble();
      final point = Offset(x(freq), y(response(freq)));
      i == 0 ? line.moveTo(point.dx, point.dy) : line.lineTo(point.dx, point.dy);
    }

    final area = Path.from(line)
      ..lineTo(size.width, y(0))
      ..lineTo(0, y(0))
      ..close();
    canvas.drawPath(area, Paint()..color = c.amberGlow);
    canvas.drawPath(
      line,
      Paint()
        ..color = c.amber
        ..style = PaintingStyle.stroke
        ..strokeWidth = 2
        ..strokeJoin = StrokeJoin.round,
    );

    for (var i = 0; i < _bands.length; i++) {
      final freq = _bands[i].$2;
      canvas.drawCircle(
        Offset(x(freq), y(response(freq))),
        3,
        Paint()..color = gains[i] < 0 ? c.magenta : c.amber,
      );
    }
  }

  @override
  bool shouldRepaint(_CurvePainter old) => old.gains != gains || old.c != c;
}

/// One fader per band, with a 0 dB detent line and the gain above it
class EqFaders extends StatelessWidget {
  const EqFaders({
    super.key,
    required this.gains,
    required this.onChanged,
    required this.onChangeEnd,
  });

  final List<int> gains;
  final ValueChanged<List<int>> onChanged;
  final ValueChanged<List<int>> onChangeEnd;

  List<int> _with(int band, double value) => [...gains]..[band] = value.round();

  @override
  Widget build(BuildContext context) {
    final c = context.colors;
    final sliderTheme = SliderTheme.of(context).copyWith(
      trackHeight: 4,
      activeTrackColor: c.line,
      inactiveTrackColor: c.line,
      thumbShape: _FaderCap(c),
      overlayColor: c.amberGlow,
      overlayShape: const RoundSliderOverlayShape(overlayRadius: 20),
      tickMarkShape: SliderTickMarkShape.noTickMark,
      showValueIndicator: ShowValueIndicator.never,
    );

    return Row(
      children: [
        for (var i = 0; i < gains.length; i++)
          Expanded(
            child: Column(
              children: [
                Text(
                  '${gains[i] > 0 ? '+' : ''}${gains[i]} dB',
                  style: c.mono(
                    color: gains[i] > 0
                        ? c.amber
                        : gains[i] < 0
                        ? c.magenta
                        : c.ink,
                  ),
                ),
                const SizedBox(height: 8),
                Expanded(
                  child: Stack(
                    alignment: Alignment.center,
                    children: [
                      Container(width: 22, height: 1, color: c.line),
                      RotatedBox(
                        quarterTurns: 3,
                        child: SliderTheme(
                          data: sliderTheme,
                          child: Slider(
                            value: gains[i].toDouble(),
                            min: -gainMaxDb.toDouble(),
                            max: gainMaxDb.toDouble(),
                            divisions: 2 * gainMaxDb,
                            onChanged: (v) => onChanged(_with(i, v)),
                            onChangeEnd: (v) => onChangeEnd(_with(i, v)),
                          ),
                        ),
                      ),
                    ],
                  ),
                ),
                const SizedBox(height: 8),
                Text(bandLabels[i], style: c.mono(size: 10, color: c.muted)),
              ],
            ),
          ),
      ],
    );
  }
}

// Drawn in the slider's own coordinates, which the faders rotate by a quarter turn
class _FaderCap extends SliderComponentShape {
  const _FaderCap(this.c);

  final AppColors c;

  static const _size = Size(18, 34);

  @override
  Size getPreferredSize(bool isEnabled, bool isDiscrete) => _size;

  @override
  void paint(
    PaintingContext context,
    Offset center, {
    required Animation<double> activationAnimation,
    required Animation<double> enableAnimation,
    required bool isDiscrete,
    required TextPainter labelPainter,
    required RenderBox parentBox,
    required SliderThemeData sliderTheme,
    required TextDirection textDirection,
    required double value,
    required double textScaleFactor,
    required Size sizeWithOverflow,
  }) {
    final canvas = context.canvas;
    final rect = RRect.fromRectAndRadius(
      Rect.fromCenter(center: center, width: _size.width, height: _size.height),
      const Radius.circular(4),
    );

    canvas.drawShadow(Path()..addRRect(rect), Colors.black, 2, false);
    canvas.drawRRect(rect, Paint()..color = c.surface);
    canvas.drawRRect(
      rect,
      Paint()
        ..color = c.line
        ..style = PaintingStyle.stroke,
    );
    canvas.drawLine(
      center.translate(0, -_size.height / 2 + 6),
      center.translate(0, _size.height / 2 - 6),
      Paint()
        ..color = c.amber
        ..strokeWidth = 2,
    );
  }
}
