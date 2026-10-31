import 'package:flutter/material.dart';
import 'package:universal_ble/universal_ble.dart';

import 'audiolink.dart';
import 'firmware_update.dart';
import 'theme.dart';
import 'header.dart';

const _stepLabels = {
  UpdateStep.application: 'Application core',
  UpdateStep.controller: 'Bluetooth controller',
  UpdateStep.confirm: 'Mark for install',
  UpdateStep.restart: 'Restart board',
};

/// Runs the firmware update and pops with a message for the user when it ends
class UpdatePage extends StatefulWidget {
  const UpdatePage({super.key, required this.device, required this.firmware});

  final BleDevice device;
  final FirmwareStatus firmware;

  @override
  State<UpdatePage> createState() => _UpdatePageState();
}

class _UpdatePageState extends State<UpdatePage> {
  var _step = UpdateStep.application;
  var _progress = 0.0;

  @override
  void initState() {
    super.initState();
    _run();
  }

  Future<void> _run() async {
    String message;
    try {
      await updateFirmware(widget.device, (step, progress) {
        if (mounted) {
          setState(() {
            _step = step;
            _progress = progress;
          });
        }
      });
      message =
          'Firmware ${widget.firmware.bundled} uploaded, $deviceName restarts and installs it';
    } catch (e) {
      debugPrint('Firmware update failed: $e');
      message = 'Firmware update failed: $e';
    }

    if (mounted) {
      Navigator.pop(context, message);
    }
  }

  @override
  Widget build(BuildContext context) {
    final c = context.colors;

    return PopScope(
      canPop: false,
      child: Scaffold(
        body: SafeArea(
          child: Center(
            child: ConstrainedBox(
              constraints: const BoxConstraints(maxWidth: 480),
              child: Column(
                children: [
                  Header(
                    status: Text('UPDATING', style: c.mono(size: 10, color: c.amber)),
                  ),
                  Expanded(
                    child: Padding(
                      padding: const EdgeInsets.all(24),
                      child: Column(
                        mainAxisAlignment: MainAxisAlignment.center,
                        children: [
                          _Ring(progress: _progress, firmware: widget.firmware),
                          const SizedBox(height: 28),
                          ConstrainedBox(
                            constraints: const BoxConstraints(maxWidth: 260),
                            child: Column(
                              children: [
                                for (final step in UpdateStep.values)
                                  _StepRow(
                                    label: _stepLabels[step]!,
                                    done: step.index < _step.index,
                                    active: step == _step,
                                  ),
                              ],
                            ),
                          ),
                          const SizedBox(height: 20),
                          Text(
                            'Keep the app open. Audio stops for a few seconds when the board '
                            'restarts.',
                            textAlign: TextAlign.center,
                            style: TextStyle(fontSize: 12, color: c.muted),
                          ),
                        ],
                      ),
                    ),
                  ),
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }
}

class _Ring extends StatelessWidget {
  const _Ring({required this.progress, required this.firmware});

  final double progress;
  final FirmwareStatus firmware;

  @override
  Widget build(BuildContext context) {
    final c = context.colors;

    return SizedBox.square(
      dimension: 170,
      child: Stack(
        fit: StackFit.expand,
        children: [
          CircularProgressIndicator(
            value: progress,
            strokeWidth: 10,
            strokeCap: StrokeCap.round,
            color: c.amber,
            backgroundColor: c.well,
          ),
          Column(
            mainAxisAlignment: MainAxisAlignment.center,
            children: [
              Text(
                '${(progress * 100).floor()}%',
                style: TextStyle(
                  fontFamily: displayFont,
                  fontSize: 30,
                  color: c.ink,
                  fontFeatures: const [FontFeature.tabularFigures()],
                ),
              ),
              const SizedBox(height: 6),
              Text(
                '${firmware.running} → ${firmware.bundled}',
                style: c.mono(size: 10, color: c.muted),
              ),
            ],
          ),
        ],
      ),
    );
  }
}

class _StepRow extends StatelessWidget {
  const _StepRow({required this.label, required this.done, required this.active});

  final String label;
  final bool done;
  final bool active;

  @override
  Widget build(BuildContext context) {
    final c = context.colors;

    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 5),
      child: Row(
        children: [
          Container(
            width: 10,
            height: 10,
            decoration: BoxDecoration(
              shape: BoxShape.circle,
              color: done
                  ? c.ok
                  : active
                  ? c.amberGlow
                  : null,
              border: Border.all(
                color: done
                    ? c.ok
                    : active
                    ? c.amber
                    : c.line,
                width: 1.5,
              ),
            ),
          ),
          const SizedBox(width: 14),
          Text(
            label,
            style: TextStyle(
              fontSize: 13,
              fontWeight: FontWeight.w500,
              color: done || active ? c.ink : c.muted,
            ),
          ),
        ],
      ),
    );
  }
}
