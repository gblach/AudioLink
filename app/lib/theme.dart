import 'package:flutter/material.dart';

const displayFont = 'Michroma';
const bodyFont = 'InstrumentSans';
const monoFont = 'MartianMono';

/// The board's faceplate: an anodized panel, and the amber and magenta its LEDs show
@immutable
class AppColors extends ThemeExtension<AppColors> {
  const AppColors({
    required this.page,
    required this.panel,
    required this.surface,
    required this.well,
    required this.line,
    required this.ink,
    required this.muted,
    required this.amber,
    required this.amberGlow,
    required this.magenta,
    required this.magentaGlow,
    required this.ok,
  });

  final Color page;
  final Color panel;
  final Color surface;
  final Color well;
  final Color line;
  final Color ink;
  final Color muted;
  final Color amber;
  final Color amberGlow;
  final Color magenta;
  final Color magentaGlow;
  final Color ok;

  static const light = AppColors(
    page: Color(0xffeef0f3),
    panel: Color(0xffe6e9ed),
    surface: Color(0xfff8f9fb),
    well: Color(0xffdde1e7),
    line: Color(0xffc4cad3),
    ink: Color(0xff1b1f27),
    muted: Color(0xff5a6271),
    amber: Color(0xffc25a00),
    amberGlow: Color(0x38ff8000),
    magenta: Color(0xffa8239a),
    magentaGlow: Color(0x33e046dc),
    ok: Color(0xff23824f),
  );

  static const dark = AppColors(
    page: Color(0xff111419),
    panel: Color(0xff191d24),
    surface: Color(0xff212631),
    well: Color(0xff151920),
    line: Color(0xff313847),
    ink: Color(0xffe9e7e2),
    muted: Color(0xff8d95a5),
    amber: Color(0xffff9a3c),
    amberGlow: Color(0x47ff8000),
    magenta: Color(0xffea5fe6),
    magentaGlow: Color(0x3dea5fe6),
    ok: Color(0xff4cc584),
  );

  /// Short engraved labels, like the printing on audio gear
  TextStyle get label =>
      TextStyle(fontFamily: displayFont, fontSize: 9, letterSpacing: 1.6, color: muted);

  TextStyle mono({double size = 11, Color? color}) => TextStyle(
    fontFamily: monoFont,
    fontSize: size,
    fontWeight: FontWeight.w500,
    color: color ?? ink,
    fontFeatures: const [FontFeature.tabularFigures()],
  );

  @override
  AppColors copyWith() => this;

  @override
  AppColors lerp(AppColors? other, double t) => t < 0.5 || other == null ? this : other;
}

extension AppColorsContext on BuildContext {
  AppColors get colors => Theme.of(this).extension<AppColors>()!;
}

ThemeData buildTheme(Brightness brightness) {
  final c = brightness == Brightness.dark ? AppColors.dark : AppColors.light;
  final scheme = ColorScheme(
    brightness: brightness,
    primary: c.amber,
    onPrimary: c.page,
    secondary: c.magenta,
    onSecondary: c.page,
    error: c.magenta,
    onError: c.page,
    surface: c.panel,
    onSurface: c.ink,
    surfaceContainerHigh: c.surface,
    outline: c.line,
  );

  return ThemeData(
    colorScheme: scheme,
    fontFamily: bodyFont,
    scaffoldBackgroundColor: c.panel,
    extensions: [c],
    filledButtonTheme: FilledButtonThemeData(
      style: FilledButton.styleFrom(
        textStyle: const TextStyle(fontFamily: bodyFont, fontWeight: FontWeight.w600),
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(8)),
      ),
    ),
    outlinedButtonTheme: OutlinedButtonThemeData(
      style: OutlinedButton.styleFrom(
        foregroundColor: c.ink,
        side: BorderSide(color: c.line),
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(8)),
      ),
    ),
    dialogTheme: DialogThemeData(
      backgroundColor: c.surface,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(16)),
    ),
    snackBarTheme: SnackBarThemeData(
      behavior: SnackBarBehavior.floating,
      backgroundColor: c.ink,
      contentTextStyle: TextStyle(fontFamily: bodyFont, color: c.page),
    ),
  );
}
