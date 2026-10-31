import 'package:flutter/material.dart';

import 'theme.dart';

/// The app's top row: the AudioLink wordmark and a short status on the right
class Header extends StatelessWidget {
  const Header({super.key, required this.status});

  final Widget status;

  @override
  Widget build(BuildContext context) {
    final c = context.colors;
    const style = TextStyle(fontFamily: displayFont, fontSize: 15, letterSpacing: 1.2);

    return Padding(
      padding: const EdgeInsets.fromLTRB(18, 16, 18, 12),
      child: Row(
        children: [
          Text.rich(
            TextSpan(
              style: style.copyWith(color: c.ink),
              children: [
                const TextSpan(text: 'AUDIO'),
                TextSpan(
                  text: 'LINK',
                  style: TextStyle(color: c.amber),
                ),
              ],
            ),
          ),
          const Spacer(),
          status,
        ],
      ),
    );
  }
}

/// Green dot and a mono caption, for example "CONNECTED · 0.0.3"
class ConnectionStatus extends StatelessWidget {
  const ConnectionStatus({super.key, required this.text});

  final String text;

  @override
  Widget build(BuildContext context) {
    final c = context.colors;

    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        Container(
          width: 7,
          height: 7,
          decoration: BoxDecoration(
            shape: BoxShape.circle,
            color: c.ok,
            boxShadow: [BoxShadow(color: c.ok.withValues(alpha: 0.3), spreadRadius: 3)],
          ),
        ),
        const SizedBox(width: 8),
        Text(text, style: c.mono(size: 10, color: c.muted)),
      ],
    );
  }
}
