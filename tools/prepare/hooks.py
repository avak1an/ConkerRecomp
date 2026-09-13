"""Insert authored host-entry guards into locally generated translation units."""
import re


def apply_entry_hooks(directory, hooks):
    pending = {}
    for hook in hooks:
        address, handler = hook['address'], hook['handler']
        if not re.fullmatch(r'[0-9A-F]{8}', address) or not re.fullmatch(r'conker_input_[a-z_]+', handler):
            raise ValueError('Invalid host entry hook metadata')
        anchor = f'void sub_{address}(void)\n{{\n'
        matches = []
        for path in directory.glob('recomp_*.c'):
            text = pending.get(path)
            if text is None:
                text = path.read_text(encoding='utf-8')
            if anchor in text:
                matches.append((path, text))
        if len(matches) != 1:
            raise ValueError(f'Expected one locally translated entry for {address}')
        path, text = matches[0]
        guard = f'    if ({handler}()) return; /* host gamepad backend */\n'
        if anchor + guard not in text:
            text = text.replace(anchor, anchor + guard, 1)
        include = '#include "input/conker_input.h"\n'
        if include not in text:
            marker = '#include "recomp_funcs.h"\n'
            if marker not in text:
                raise ValueError('Missing generated header')
            text = text.replace(marker, marker + include, 1)
        pending[path] = text
    for path, text in pending.items():
        if text != path.read_text(encoding='utf-8'):
            path.write_text(text, encoding='utf-8', newline='\n')
