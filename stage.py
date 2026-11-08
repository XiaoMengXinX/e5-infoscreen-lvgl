"""Stage the shared info screen with the LVGL program as its interface."""
from pathlib import Path
import runpy

HERE = Path(__file__).resolve().parent


def stage(root):
    root = Path(root)
    build = runpy.run_path(str(HERE / 'build.py'))
    runpy.run_path(str(build['E5_LINUX'] / 'devices/common/infoscreen/stage.py'))['stage'](root)
    binary = build['build']()
    libexec = root / 'usr/libexec/e5-infoscreen'
    # The web interface's session first, kept for switch-ui to switch back to.
    for source, name in [(libexec / 'session', 'session.webkit'), (binary, 'ui'), (HERE / 'session', 'session'),
                         (HERE / 'scripts/switch-ui.sh', 'switch-ui')]:
        dest = libexec / name
        dest.write_bytes(Path(source).read_bytes())
        dest.chmod(0o755)


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root')
    stage(parser.parse_args().root)
