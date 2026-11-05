import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import 'audiolink.dart';
import 'eq_view.dart';
import 'firmware_update.dart';
import 'header.dart';
import 'status_view.dart';
import 'theme.dart';
import 'update_page.dart';

void main() {
  LicenseRegistry.addLicense(() async* {
    for (final (font, file) in [
      ('Michroma', 'OFL-Michroma.txt'),
      ('Instrument Sans', 'OFL-InstrumentSans.txt'),
      ('Martian Mono', 'OFL-MartianMono.txt'),
    ]) {
      yield LicenseEntryWithLineBreaks([font], await rootBundle.loadString('fonts/$file'));
    }
  });
  runApp(const App());
}

class App extends StatelessWidget {
  const App({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'AudioLink',
      theme: buildTheme(Brightness.light),
      darkTheme: buildTheme(Brightness.dark),
      home: const EqPage(),
    );
  }
}

class EqPage extends StatefulWidget {
  const EqPage({super.key});

  @override
  State<EqPage> createState() => _EqPageState();
}

class _EqPageState extends State<EqPage> {
  AudioLink? _link;
  StreamSubscription<bool>? _connectionSub;
  List<int>? _gains;
  int? _ledMax;
  int? _ledTheme;
  bool? _test;
  FirmwareStatus? _firmware;
  String? _error;

  @override
  void initState() {
    super.initState();
    _connect();
  }

  @override
  void dispose() {
    _connectionSub?.cancel();
    _link?.disconnect();
    super.dispose();
  }

  Future<void> _connect() async {
    setState(() {
      _error = null;
      _gains = null;
    });

    try {
      final link = await AudioLink.connect();
      final gains = await link.readGains();
      final ledMax = await link.readLedMax();
      final ledTheme = await link.readLedTheme();
      final test = await link.readTest();
      final firmware = await _checkFirmware(link);
      if (!mounted) {
        return;
      }

      _connectionSub?.cancel();
      _connectionSub = link.connectionStream.listen((connected) {
        if (!connected && mounted) {
          setState(() {
            _gains = null;
            _error = 'The connection to $deviceName dropped.';
          });
        }
      });
      setState(() {
        _link = link;
        _gains = gains;
        _ledMax = ledMax;
        _ledTheme = ledTheme;
        _test = test;
        _firmware = firmware;
      });
    } on TimeoutException {
      _fail('$deviceName was not found. Power it on and pair it in the Bluetooth settings.');
    } catch (e) {
      _fail('$e');
    }
  }

  // The EQ works without the update service, so a failed check only hides the version
  Future<FirmwareStatus?> _checkFirmware(AudioLink link) async {
    try {
      return await firmwareStatus(link.device);
    } catch (e) {
      debugPrint('Firmware check failed: $e');
      return null;
    }
  }

  void _fail(String message) {
    debugPrint('Connect failed: $message');
    if (mounted) {
      setState(() => _error = message);
    }
  }

  void _showMessage(String message) {
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(message)));
  }

  void _showWriteError(Object e) {
    debugPrint('Write failed: $e');
    _showMessage('Write failed: $e');
  }

  Future<void> _write(List<int> gains) async {
    setState(() => _gains = gains);

    try {
      await _link!.writeGains(gains);
    } catch (e) {
      if (!mounted) {
        return;
      }
      _showWriteError(e);
      // Show what the board actually uses, not what failed to apply
      try {
        final current = await _link!.readGains();
        if (mounted) {
          setState(() => _gains = current);
        }
      } catch (_) {}
    }
  }

  Future<void> _writeLedMax(int percent) async {
    setState(() => _ledMax = percent);

    try {
      await _link!.writeLedMax(percent);
    } catch (e) {
      if (!mounted) {
        return;
      }
      _showWriteError(e);
      try {
        final current = await _link!.readLedMax();
        if (mounted) {
          setState(() => _ledMax = current);
        }
      } catch (_) {}
    }
  }

  Future<void> _writeLedTheme(int theme) async {
    setState(() => _ledTheme = theme);

    try {
      await _link!.writeLedTheme(theme);
    } catch (e) {
      if (!mounted) {
        return;
      }
      _showWriteError(e);
      try {
        final current = await _link!.readLedTheme();
        if (mounted) {
          setState(() => _ledTheme = current);
        }
      } catch (_) {}
    }
  }

  Future<void> _writeTest(bool on) async {
    setState(() => _test = on);

    try {
      await _link!.writeTest(on);
    } catch (e) {
      if (!mounted) {
        return;
      }
      _showWriteError(e);
      try {
        final current = await _link!.readTest();
        if (mounted) {
          setState(() => _test = current);
        }
      } catch (_) {}
    }
  }

  Future<void> _updateFirmware() async {
    final firmware = _firmware!;
    final start = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Install firmware'),
        content: Text(
          'Installs firmware ${firmware.bundled} on $deviceName. This takes a few minutes, '
          'keep the app open. The board then restarts and audio stops for a few seconds.',
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context, false), child: const Text('Cancel')),
          FilledButton(onPressed: () => Navigator.pop(context, true), child: const Text('Install')),
        ],
      ),
    );
    if (start != true || !mounted) {
      return;
    }

    final message = await Navigator.push<String>(
      context,
      MaterialPageRoute(
        builder: (context) => UpdatePage(device: _link!.device, firmware: firmware),
      ),
    );
    if (message != null && mounted) {
      _showMessage(message);
    }
  }

  @override
  Widget build(BuildContext context) {
    final c = context.colors;
    final gains = _gains;
    final ledMax = _ledMax;
    final ledTheme = _ledTheme;
    final test = _test;
    final firmware = _firmware;
    final connected = gains != null && ledMax != null;

    return Scaffold(
      body: SafeArea(
        child: Center(
          child: ConstrainedBox(
            constraints: const BoxConstraints(maxWidth: 480),
            child: Column(
              children: [
                Header(
                  status: connected
                      ? ConnectionStatus(
                          text: firmware == null ? 'CONNECTED' : 'CONNECTED · ${firmware.running}',
                        )
                      : Text(
                          _error == null ? 'SEARCHING' : 'OFFLINE',
                          style: c.mono(size: 10, color: c.amber),
                        ),
                ),
                if (!connected)
                  Expanded(
                    child: StatusView(error: _error, onRetry: _connect),
                  )
                else ...[
                  if (firmware != null && firmware.updateAvailable)
                    _UpdateBanner(version: firmware.bundled, onInstall: _updateFirmware),
                  const SizedBox(height: 14),
                  _SectionLabel(
                    text: 'EQUALIZER',
                    action: _SmallButton(
                      label: 'FLAT',
                      onPressed: () => _write(List.filled(gains.length, 0)),
                    ),
                  ),
                  Padding(
                    padding: const EdgeInsets.fromLTRB(18, 8, 18, 0),
                    child: EqCurve(gains: gains),
                  ),
                  Expanded(
                    child: Padding(
                      padding: const EdgeInsets.fromLTRB(10, 14, 10, 8),
                      child: EqFaders(
                        gains: gains,
                        onChanged: (gains) => setState(() => _gains = gains),
                        onChangeEnd: _write,
                      ),
                    ),
                  ),
                  const _SectionLabel(text: 'LED BRIGHTNESS'),
                  _LedCard(
                    percent: ledMax,
                    colors: _themeColors(c)[ledTheme ?? 0],
                    onChanged: (percent) => setState(() => _ledMax = percent),
                    onChangeEnd: _writeLedMax,
                  ),
                  if (ledTheme != null) ...[
                    const _SectionLabel(text: 'LED THEME'),
                    _ThemePicker(theme: ledTheme, onSelected: _writeLedTheme),
                  ],
                  if (test != null)
                    Padding(
                      padding: const EdgeInsets.only(bottom: 10),
                      child: _SectionLabel(
                        text: 'TEST NOISE',
                        action: Switch(value: test, onChanged: _writeTest),
                      ),
                    ),
                ],
              ],
            ),
          ),
        ),
      ),
    );
  }
}

class _UpdateBanner extends StatelessWidget {
  const _UpdateBanner({required this.version, required this.onInstall});

  final String version;
  final VoidCallback onInstall;

  @override
  Widget build(BuildContext context) {
    final c = context.colors;

    return Container(
      margin: const EdgeInsets.symmetric(horizontal: 18),
      padding: const EdgeInsets.fromLTRB(12, 6, 6, 6),
      decoration: BoxDecoration(
        color: c.amberGlow,
        borderRadius: BorderRadius.circular(10),
        border: Border.all(color: c.amber.withValues(alpha: 0.45)),
      ),
      child: Row(
        children: [
          Expanded(
            child: Text.rich(
              TextSpan(
                style: TextStyle(fontSize: 13, fontWeight: FontWeight.w500, color: c.ink),
                children: [
                  const TextSpan(text: 'Firmware '),
                  TextSpan(
                    text: version,
                    style: const TextStyle(fontWeight: FontWeight.w600),
                  ),
                  const TextSpan(text: ' is ready to install'),
                ],
              ),
            ),
          ),
          FilledButton(
            onPressed: onInstall,
            style: FilledButton.styleFrom(
              visualDensity: VisualDensity.compact,
              shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(6)),
            ),
            child: const Text('Install'),
          ),
        ],
      ),
    );
  }
}

class _SectionLabel extends StatelessWidget {
  const _SectionLabel({required this.text, this.action});

  final String text;
  final Widget? action;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 18),
      child: Row(
        children: [
          Text(text, style: context.colors.label),
          const Spacer(),
          ?action,
        ],
      ),
    );
  }
}

class _SmallButton extends StatelessWidget {
  const _SmallButton({required this.label, required this.onPressed});

  final String label;
  final VoidCallback onPressed;

  @override
  Widget build(BuildContext context) {
    final c = context.colors;

    return OutlinedButton(
      onPressed: onPressed,
      style: OutlinedButton.styleFrom(
        foregroundColor: c.amber,
        visualDensity: VisualDensity.compact,
        padding: const EdgeInsets.symmetric(horizontal: 10),
        minimumSize: const Size(0, 30),
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(5)),
      ),
      child: Text(label, style: c.mono(size: 10, color: c.amber)),
    );
  }
}

// The firmware's LED themes in its order: the color of LEDs 0 and 2, then of LEDs 1 and 3
List<(Color, Color)> _themeColors(AppColors c) => [
  (c.amber, c.magenta),
  (const Color(0xff0000ff), const Color(0xff00ffff)),
  (const Color(0xff00ff00), const Color(0xff80ff00)),
  (c.amber, const Color(0xfffff000)),
];

class _LedCard extends StatelessWidget {
  const _LedCard({
    required this.percent,
    required this.colors,
    required this.onChanged,
    required this.onChangeEnd,
  });

  final int percent;
  final (Color, Color) colors;
  final ValueChanged<int> onChanged;
  final ValueChanged<int> onChangeEnd;

  @override
  Widget build(BuildContext context) {
    final c = context.colors;
    final glow = 0.25 + 0.75 * percent / 100;

    Widget led(Color color) => Container(
      width: 9,
      height: 9,
      decoration: BoxDecoration(
        shape: BoxShape.circle,
        color: color.withValues(alpha: glow),
        boxShadow: [BoxShadow(color: color.withValues(alpha: glow * 0.5), blurRadius: 8)],
      ),
    );

    return Container(
      margin: const EdgeInsets.fromLTRB(18, 8, 18, 18),
      padding: const EdgeInsets.fromLTRB(14, 4, 14, 4),
      decoration: BoxDecoration(
        color: c.surface,
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: c.line),
      ),
      child: Row(
        children: [
          // The same colors in the same positions as the four LEDs on the board
          Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              Row(children: [led(colors.$1), const SizedBox(width: 5), led(colors.$2)]),
              const SizedBox(height: 5),
              Row(children: [led(colors.$1), const SizedBox(width: 5), led(colors.$2)]),
            ],
          ),
          Expanded(
            child: SliderTheme(
              data: SliderTheme.of(context).copyWith(
                activeTrackColor: c.amber,
                inactiveTrackColor: c.well,
                thumbColor: c.amber,
                overlayColor: c.amberGlow,
              ),
              child: Slider(
                value: percent.toDouble(),
                max: 100,
                divisions: 20,
                onChanged: (v) => onChanged(v.round()),
                onChangeEnd: (v) => onChangeEnd(v.round()),
              ),
            ),
          ),
          SizedBox(
            width: 44,
            child: Text('$percent %', textAlign: TextAlign.end, style: c.mono()),
          ),
        ],
      ),
    );
  }
}

class _ThemePicker extends StatelessWidget {
  const _ThemePicker({required this.theme, required this.onSelected});

  final int theme;
  final ValueChanged<int> onSelected;

  @override
  Widget build(BuildContext context) {
    final c = context.colors;
    final themes = _themeColors(c);

    Widget dot(Color color) => Container(
      width: 12,
      height: 12,
      decoration: BoxDecoration(
        shape: BoxShape.circle,
        color: color,
        border: Border.all(color: c.line),
      ),
    );

    return Padding(
      padding: const EdgeInsets.fromLTRB(18, 8, 18, 18),
      child: Row(
        children: [
          for (var i = 0; i < themes.length; i++) ...[
            if (i > 0) const SizedBox(width: 8),
            Expanded(
              child: InkWell(
                onTap: () => onSelected(i),
                borderRadius: BorderRadius.circular(10),
                child: Container(
                  height: 40,
                  decoration: BoxDecoration(
                    color: c.surface,
                    borderRadius: BorderRadius.circular(10),
                    border: Border.all(
                      color: i == theme ? c.amber : c.line,
                      width: i == theme ? 2 : 1,
                    ),
                  ),
                  child: Row(
                    mainAxisAlignment: MainAxisAlignment.center,
                    children: [dot(themes[i].$1), const SizedBox(width: 5), dot(themes[i].$2)],
                  ),
                ),
              ),
            ),
          ],
        ],
      ),
    );
  }
}
