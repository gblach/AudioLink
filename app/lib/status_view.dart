import 'dart:math';

import 'package:flutter/material.dart';

import 'audiolink.dart';
import 'theme.dart';

/// Shown while connecting, and when the board is not found or the connection dropped
class StatusView extends StatelessWidget {
  const StatusView({super.key, required this.error, required this.onRetry});

  final String? error;
  final VoidCallback onRetry;

  @override
  Widget build(BuildContext context) {
    final c = context.colors;

    return Center(
      child: Padding(
        padding: const EdgeInsets.all(24),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            _Board(searching: error == null),
            const SizedBox(height: 8),
            Text(
              error == null ? 'Looking for $deviceName' : 'Not connected',
              textAlign: TextAlign.center,
              style: TextStyle(
                fontFamily: displayFont,
                fontSize: 15,
                letterSpacing: 0.6,
                color: c.ink,
              ),
            ),
            const SizedBox(height: 10),
            ConstrainedBox(
              constraints: const BoxConstraints(maxWidth: 280),
              child: Text(
                error ??
                    'Power the board from USB-C and pair it in your phone\'s Bluetooth settings '
                        'first.',
                textAlign: TextAlign.center,
                style: TextStyle(fontSize: 13, color: c.muted),
              ),
            ),
            if (error != null) ...[
              const SizedBox(height: 20),
              OutlinedButton(onPressed: onRetry, child: const Text('Try again')),
            ],
          ],
        ),
      ),
    );
  }
}

class _Board extends StatefulWidget {
  const _Board({required this.searching});

  final bool searching;

  @override
  State<_Board> createState() => _BoardState();
}

class _BoardState extends State<_Board> with SingleTickerProviderStateMixin {
  late final _pulse = AnimationController(
    vsync: this,
    duration: const Duration(milliseconds: 2400),
  );

  void _animate() {
    if (widget.searching && !MediaQuery.disableAnimationsOf(context)) {
      _pulse.repeat();
    } else {
      _pulse.stop();
    }
  }

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    _animate();
  }

  @override
  void didUpdateWidget(_Board old) {
    super.didUpdateWidget(old);
    _animate();
  }

  @override
  void dispose() {
    _pulse.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      width: _BoardPainter.area.width,
      height: _BoardPainter.area.height,
      child: CustomPaint(painter: _BoardPainter(_pulse, widget.searching, context.colors)),
    );
  }
}

// The board seen from above, jacks on the right and the four status LEDs on the long edges.
// While searching, rings spread out from it like a radio signal.
class _BoardPainter extends CustomPainter {
  _BoardPainter(this.pulse, this.searching, this.c) : super(repaint: pulse);

  final Animation<double> pulse;
  final bool searching;
  final AppColors c;

  static const area = Size(300, 200);
  static const _board = Size(200, 110);
  static const _ringSpread = 46.0;
  static const _rings = 3;

  @override
  void paint(Canvas canvas, Size size) {
    final origin = Offset((size.width - _board.width) / 2, (size.height - _board.height) / 2);
    final board = RRect.fromRectAndRadius(origin & _board, const Radius.circular(10));

    if (searching) {
      for (var i = 0; i < _rings; i++) {
        final t = (pulse.value + i / _rings) % 1;
        canvas.drawRRect(
          board.inflate(t * _ringSpread),
          Paint()
            ..color = c.amber.withValues(alpha: (1 - t) * 0.55)
            ..style = PaintingStyle.stroke
            ..strokeWidth = 1.5,
        );
      }
    }

    canvas.drawRRect(board, Paint()..color = Color.lerp(c.surface, c.ok, 0.14)!);
    canvas.drawRRect(
      board,
      Paint()
        ..color = c.line
        ..style = PaintingStyle.stroke,
    );

    canvas.save();
    canvas.translate(origin.dx, origin.dy);
    final parts = Paint()..color = c.line;
    final w = _board.width;
    final h = _board.height;
    for (final jack in [
      Rect.fromLTWH(w - 16, 14, 22, 16),
      Rect.fromLTWH(w - 10, h / 2 - 6, 14, 12),
      Rect.fromLTWH(w - 16, h - 30, 22, 16),
    ]) {
      canvas.drawRRect(RRect.fromRectAndRadius(jack, const Radius.circular(3)), parts);
    }
    canvas.drawRRect(
      RRect.fromRectAndRadius(const Rect.fromLTWH(22, 36, 46, 38), const Radius.circular(3)),
      parts,
    );

    final leds = [
      (Offset(34, 0), c.amber),
      (Offset(w - 50, 0), c.magenta),
      (Offset(34, h), c.amber),
      (Offset(w - 50, h), c.magenta),
    ];
    for (var i = 0; i < leds.length; i++) {
      final (center, color) = leds[i];
      final phase = (pulse.value - i * 0.125) * 2 * pi;
      final alpha = searching ? 0.35 + 0.65 * (0.5 - 0.5 * cos(phase)) : 0.35;
      canvas.drawCircle(
        center,
        9,
        Paint()
          ..color = color.withValues(alpha: alpha * 0.5)
          ..maskFilter = const MaskFilter.blur(BlurStyle.normal, 6),
      );
      canvas.drawCircle(center, 4, Paint()..color = color.withValues(alpha: alpha));
    }
    canvas.restore();
  }

  @override
  bool shouldRepaint(_BoardPainter old) => old.searching != searching || old.c != c;
}
