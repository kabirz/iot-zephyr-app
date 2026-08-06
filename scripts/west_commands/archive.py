# Copyright (c) 2026 Kabirz
#
# SPDX-License-Identifier: Apache-2.0

import argparse
import subprocess
import sys
import textwrap
import zipfile
from pathlib import Path
import shutil

from west.commands import WestCommand
from yaml import safe_load



class Archive(WestCommand):
    def __init__(self):
        super().__init__(
            "archive",
            "manage output for Zephyr",
            "Install all output into zip",
            accepts_unknown_args=True,
        )

    @staticmethod
    def _resolve_script(name):
        """Resolve a Python script to an executable path (cross-platform)."""
        # Linux: scripts with shebang are directly executable via PATH
        path = shutil.which(name)
        if path:
            return [path]
        # Windows: .py files are not in PATHEXT, look in Python's Scripts dir
        scripts_dir = Path(sys.executable).parent
        script_path = scripts_dir / name
        if script_path.exists():
            return [sys.executable, str(script_path)]
        return [name]

    def do_add_parser(self, parser_adder):
        parser = parser_adder.add_parser(
            self.name,
            help=self.help,
            description=self.description,
            formatter_class=argparse.RawDescriptionHelpFormatter,
            epilog=textwrap.dedent(
                """
                archive all output image and other file
                to zip file.
            """
            ),
        )

        parser.add_argument('-d', '--build-dir', default='build',
                            help='build directory to create or use')
        parser.add_argument('--rebuild', action=argparse.BooleanOptionalAction,
                            help='manually specify to reinvoke cmake or not')
        parser.add_argument('--extra-file', nargs='*', help='add list file to zip')
        parser.add_argument('-o', '--output', help='spec output zip name')

        return parser

    def do_run(self, args, _):
        build_dir = Path(args.build_dir)
        try:
            with open(build_dir/'build_info.yml') as f:
                info = safe_load(f)
        except FileNotFoundError:
            self.die("Can't found build_info.yml, Please rebuild this target")

        # rebuild target
        if args.rebuild is not False:
            _cmd = ['cmake', '--build', args.build_dir]
            proc = subprocess.run(_cmd, encoding="utf-8")
            if proc.returncode:
                self.die('build error')

        # compress file
        images_path = build_dir/'output/images'
        if images_path.exists():
            if images_path.is_dir():
                shutil.rmtree(images_path)
            else:
                images_path.unlink()
        images_path.mkdir(parents=True, exist_ok=True)
        if args.extra_file:
            for _file in args.extra_file:
                p_file = Path(_file)
                if p_file.is_dir():
                    shutil.copytree(p_file, images_path/p_file.name, dirs_exist_ok=True)
                else:
                    shutil.copy(_file, images_path)
        app_name = "app"
        board_name = "common"
        cmake_info = info.get('cmake')
        board = cmake_info.get('board')
        if board:
            board_name = board['name']
        sysbuild = cmake_info.get('sysbuild')
        if sysbuild == 'true':
            hex_file_lists = []
            for image in cmake_info.get('images'):
                if image['type'] == 'BOOTLOADER':
                    bootloader_path = build_dir/image['name']
                    image_dir = bootloader_path/'zephyr'
                    shutil.copyfile(image_dir/'zephyr.bin', images_path/'bootloader.bin')
                    shutil.copyfile(image_dir/'zephyr.hex', images_path/'bootloader.hex')
                    hex_file_lists.append(image_dir/'zephyr.hex')
                elif image['type'] == 'MAIN':
                    app_path = build_dir/image['name']
                    image_dir = app_path/'zephyr'
                    shutil.copyfile(image_dir/'zephyr.signed.bin', images_path/'app.bin')
                    shutil.copyfile(image_dir/'zephyr.signed.hex', images_path/'app.hex')
                    hex_file_lists.append(image_dir/'zephyr.signed.hex')
                    app_name = image['name']
            if hex_file_lists:
                output_hex = build_dir/'full_output.hex'
                _cmd = self._resolve_script('hexmerge.py')
                _cmd.extend(hex_file_lists)
                _cmd.extend(['-o', output_hex.as_posix()])
                proc = subprocess.run(_cmd, encoding="utf-8")
                if proc.returncode:
                    self.die('hexmerge failed')
                shutil.copy(output_hex, images_path)
                # full bin image
                _cmd = self._resolve_script('hex2bin.py')
                _cmd.extend([output_hex, output_hex.with_suffix('.bin')])
                proc = subprocess.run(_cmd, encoding="utf-8")
                if proc.returncode:
                    self.die('hex2bin failed')
                shutil.copy(output_hex.with_suffix('.bin'), images_path)
        else:
            app_info = cmake_info.get('application')
            app_name = Path(app_info['source-dir']).name
            image_dir = build_dir/'zephyr'
            if (image_dir/'zephyr.bin').exists():
                shutil.copyfile(image_dir/'zephyr.bin', images_path/'app.bin')
                shutil.copyfile(image_dir/'zephyr.hex', images_path/'app.hex')
            elif (image_dir/'zephyr.exe').exists():
                shutil.copyfile(image_dir/'zephyr.exe', images_path/'app.exe')
            elif (image_dir/'zephyr.elf').exists():
                shutil.copyfile(image_dir/'zephyr.elf', images_path/'app.elf')
        output_name = f'{board_name}_{app_name}.zip'
        if args.output:
            output_name = args.output
            if not output_name.endswith('.zip'):
                output_name = f'{output_name}.zip'
        self.zip_files(images_path.parent, build_dir/output_name)

    def zip_files(self, paths: Path, out_zip_file: Path) -> None:
        self.inf(f'Files compressed into {out_zip_file}, List:')
        with zipfile.ZipFile(out_zip_file, 'w', zipfile.ZIP_DEFLATED) as zipf:
            for file in paths.rglob('*'):
                if file.is_file():
                    zipf.write(file, file.relative_to(paths))
                    self.inf(f'    {file.relative_to(paths)}')

