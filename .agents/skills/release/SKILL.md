---
name: release
description: Use when the user asks to release/publish a firmware version for an app in this Zephyr repo (e.g. "release angle-handler to 0.1.5", "发布 n2e-gw 0.2.0", "给 angle-handler 发版 0.1.5"). 改写 app 的 VERSION 文件、提交、打 tag，然后用指定的 Python 虚拟环境编译并归档产物。需要用户提供三项输入：app 的相对路径、venv 目录路径、目标版本号（MAJOR.MINOR.PATCH）。仅用于正式发版，不要在普通编译/调试场景加载本 skill。
---

# 发布固件版本

本 skill 为本 Zephyr west manifest 仓库里的某个 app（`applications/angle-handler`
或 `applications/n2e-gw`）切出一个正式版本：改写 app 的 `VERSION` 文件、提交、
**先打 tag 再编译**、用用户指定的 Python 虚拟环境编译、最后归档镜像。必须严格按
顺序执行每一步；任何一步失败都要立即停下，并报告已经成功的步骤，方便用户回滚。

## 1. 收集参数

从用户消息中提取以下参数。若任何一个必填项缺失或含糊，先询问用户，不要动手。

- **`<app_path>`**（必填）— app 在仓库内的相对路径，如 `applications/angle-handler`。
  必须存在且包含 `VERSION` 文件。
- **`<venv_path>`**（必填）— 已安装 `west` 的 Python 虚拟环境根目录，可用相对或绝对
  路径，如 `.venv`。
- **`<version>`**（必填）— 目标版本号，`MAJOR.MINOR.PATCH` 格式，如 `0.1.5`。不匹配
  `^\d+\.\d+\.\d+$` 的要拒绝并重新询问。
- **`--board <board>`**（可选）— 覆盖目标板子，默认 `nrf24_f103rct6`。
- **`--no-sysbuild`**（可选）— 不使用 sysbuild。默认启用 sysbuild（`--sysbuild`），
  因为要打包 MCUboot。

派生量：
- **`<app_name>`** = `<app_path>` 的 basename（如 `angle-handler`）。
- **`<display_name>`** = app 的中文显示名（用于 changelog 标题、发布 zip 命名、汇报）。
  固定映射：`angle-handler` → `手柄`，`n2e-gw` → `手柄接收器`。其他 app 报错并询问用户。
- **`<tag>`** = `v<version>-<app_name>`（如 `v1.0.1-angle-handler`，**tag 用英文 app_name，
  不用中文**，避免中文在 git tag 里出问题）。
- **`<release_zip>`** = `<display_name>-v<version>.zip`（如 `手柄-v1.0.1.zip`）。
- **`<prev_tag>`** = 该 app 上一个 release tag。用
  `git tag --list 'v*-<app_name>' --sort=-v:refname` 取出该 app 所有历史 release tag，
  按版本号倒序，第一个就是上一个 release（本次的 `<tag>` 此时还没打，不会出现在列表里）。
  若列表为空，说明是首次发版。

## 2. 预检（全部通过才继续；任一失败立即中止，且不改动任何文件）

在修改任何文件之前先跑这些检查，遇到第一个失败就停下并报告。

1. **分支**：当前分支必须是 `main` 或 `master`。
   `git rev-parse --abbrev-ref HEAD`
2. **工作区干净**：没有未提交改动。
   `git status --porcelain` 输出必须为空。
3. **app 存在**：`<app_path>` 是目录且含有 `VERSION` 文件。
4. **tag 未被占用**：`<tag>` 当前不存在。
   `git rev-parse <tag>` 必须失败（返回非零）。
5. **venv 可用**：`<venv_path>` 存在，且能找到 python 解释器：
   - Windows：`<venv_path>/Scripts/python.exe`
   - POSIX：`<venv_path>/bin/python`

   用 `<python> --version` 和 `<python> -m west --version` 验证，两条都必须成功。

## 3. 改写 VERSION 文件

用下面这段 Zephyr kernel 风格内容覆盖 `<app_path>/VERSION`，把解析出的 `<version>`
各部分填进去。release 时 `EXTRAVERSION` 一律设为 `release`（固件据此区分正式版与
开发版）。

```
VERSION_MAJOR = <MAJOR>
VERSION_MINOR = <MINOR>
PATCHLEVEL = <PATCH>
VERSION_TWEAK = 0
EXTRAVERSION = release
```

保留结尾换行。不要改动其他任何文件。

## 4. 提交并打 tag（tag 打完之后才编译）

顺序是硬性要求——编译必须在打了 tag 的 commit 上进行，这样注入的 git commit hash
和 release tag 才能对上。

1. `git add <app_path>/VERSION`
2. `git commit -m "release: <app_name> v<version>"`
3. `git tag -a <tag> -m "release <app_name> v<version>"`

若提交或打 tag 失败，停下并报告（VERSION 的改动已暂存/已提交，但没打成 tag；用户
可能想 `git reset`）。

## 5. 用 venv 编译

直接通过 venv 的 python 调用 `west`（不要依赖 `activate`，那是 shell 专属的）。在
仓库根目录下执行。

- Windows：
  `<venv_path>/Scripts/python.exe -m west build -b <board> <app_path> --sysbuild`
- POSIX：
  `<venv_path>/bin/python -m west build -b <board> <app_path> --sysbuild`

若指定了 `--no-sysbuild` 则去掉 `--sysbuild`。使用默认的 build 目录 `build`（与现有
`west archive` 命令一致）。编译失败则停下并报告——此时 tag 已经存在，要把 tag 名
告诉用户，方便其修复后重跑编译，或删除 tag。

## 6. 归档产物

用同一个 venv 的 python 把刚编译出的镜像归档成 zip（复用项目自带的 `west archive`
命令）：

- Windows：
  `<venv_path>/Scripts/python.exe -m west archive --no-rebuild -o <app_name>`
- POSIX：
  `<venv_path>/bin/python -m west archive --no-rebuild -o <app_name>`

`--no-rebuild` 避免重复编译；输出 zip 在仓库根目录，命名为 `<board>_<app_name>.zip`
（受 `-o` 影响也可能是 `<app_name>.zip`）。失败则报告并停下。

`west archive` 会把所有镜像收集到 `build/output/images/`，其中 sysbuild 场景下会包含
`full_output.hex`（bootloader + app 合并镜像）和 `app.bin`（签名后的 app 镜像）。

## 7. 生成 / 追加 changelog

编译归档成功后，为本次 release 在 `<app_path>/CHANGELOG.md` 里追加一条记录。该文件
按时间倒序排列（最新版本在最上面）。**若文件不存在则创建**（首行写一级标题
`# <display_name> 变更记录`，如 `# 手柄 变更记录`，空一行后再写第一条记录）。

**确定改动范围（只看本 app 目录的提交）**：

- 若存在 `<prev_tag>`：改动区间是 `git log <prev_tag>..<tag> -- <app_path>`，即从
  上一次 release tag 之后、到本次 release tag（含）之间、**只动过 `<app_path>` 下
  文件**的提交。
- 若是首次发版（无 `<prev_tag>`）：从仓库根 commit 起算。先用
  `git rev-list --max-parents=0 HEAD` 取出仓库初始 commit `<root>`，然后
  `git log <root>..<tag> -- <app_path>`（含初始 commit 自身，因此命令改为
  `git log <root>~1..<tag> -- <app_path>` 或直接 `git log --reverse <tag> -- <app_path>`
  列出截至本次 release 的全部本 app 提交）。

**每条改动必须用中文详细描述具体内容**，不能只抄 commit message 首行。对每个提交：

1. 先取该提交的元信息：`git show -s --format='%h %s' <commit>`。
2. 再取该提交**对本 app 目录**的具体改动：`git show --stat <commit> -- <app_path>`
   看改了哪些文件；必要时用 `git show <commit> -- <文件>` 看 diff 摘要（改动的关键值、
   新增/删除了什么配置项、参数从 X 调整为 Y 等）。
3. 用一句通顺的中文概括这个提交**实际做了什么改动**（面向读 changelog 的用户，而非
   开发者）。例如：
   - 差 ❌：`d1aa6e8 adc: tune RF24 sleep interval to 50ms`（只抄英文首行）
   - 好 ✅：`d1aa6e8` 将 RF24 无线模块的休眠采样间隔从 60ms 调整为 50ms
           （`applications/angle-handler/src/adc.c`）
   - 差 ❌：`bf89def apps: sysconfig update`
   - 好 ✅：`bf89def` MCUboot 签名密钥路径改为按板子名变量引用
           `${APP_DIR}/boards/${BOARD}.pem`，避免硬编码板子名
           （`sysbuild.conf`）

**记录格式**（追加到文件顶部、标题之下；标题与各小节标题一律中文）：

```markdown
## v<version>（<提交日期>）

- 发布标签：`<tag>`
- 发布提交：`<6位短SHA>`
- 提交时间：`<YYYY-MM-DD HH:MM:SS +时区>`（release commit 的作者时间，
  `git show -s --format=%ai <tag>`）

### 自 <prev_tag 或 "仓库初始提交"> 以来的改动

- `<6位短SHA>` <中文详细描述>（`<改动的文件>`）
- `<6位短SHA>` <中文详细描述>（`<改动的文件>`）
- ...
```

要点：
- **标题用中文**：一级标题 `# <app_name> 变更记录`；版本小节标题
  `## v<version>（<日期>）`；改动小节 `### 自 ... 以来的改动`。
- **commit id 一律用 6 位短 SHA**（`git log/show --format=%h` 或
  `--abbrev-commit`），不要完整 SHA，后面也不要加括号描述。
- **每条改动用中文详细描述**具体改了什么，并附上改动的文件路径（相对仓库根）。
- **改动列表**只包含 `<app_path>` 目录下的提交，按时间倒序（最新在上）。
- 若该区间内本 app 目录没有任何提交（极少见，通常只会发生在重复打同一版本），改动列表
  写 `（无）`，不要留空。

此步骤只改 `<app_path>/CHANGELOG.md` 这一个文件；改完之后**提交**它：

```
git add <app_path>/CHANGELOG.md
git commit -m "docs: <app_name> changelog for v<version>"
```

（这一条 changelog 提交落在 release tag 之后，不影响 release tag 指向的镜像内容，
是预期行为。）若失败则报告并停下。

## 8. 打包发布镜像

从 `build/output/images/` 里取出 **`full_output.hex`** 和 **`app.bin`** 两个文件，
再从刚提交的 `<app_path>/CHANGELOG.md` 生成一个 **`版本历史.txt`**，三个文件一起打成一个
zip，命名为 **`<release_zip>`**（即 `<display_name>-v<version>.zip`，如 `手柄-v1.0.1.zip`），
放在仓库根目录。

- 两个镜像源文件必须都存在；若任一缺失（例如用了 `--no-sysbuild` 导致没有
  `full_output.hex`），停下并报告缺哪个文件。编译产物里**只有 `app.bin`，没有
  `update.bin`**——`update.bin` 仅在打包进 zip 时由 `app.bin` 改名而来，磁盘上的源文件
  保持 `app.bin` 不变。
- **打包时把 `app.bin` 改名为 `update.bin` 写入 zip**（OTA 升级镜像统一叫 update.bin）；
  `full_output.hex` 保持原名。
- **`版本历史.txt` 面向最终用户**，由 CHANGELOG.md 生成，包含该 app 的**全部历史记录**
  （不止本次版本）。但**不是机械去 markdown 标记**——要在转换时**把每条改动改写成
  面向用户的语言**。CHANGELOG.md 是给开发者看的（可含文件路径、代码、变量名、commit
  SHA），版本历史.txt 是给刷固件的最终用户看的，两者内容不同。改写规则：
  - **只讲硬件、逻辑、参数的改动**：例如把"RF24_SLEEP_MS 从 60 改成 50"写成"无线模块
    采样间隔由 60ms 缩短为 50ms"；把"MCUboot 签名密钥路径改为 `${APP_DIR}/boards/${BOARD}.pem`"
    写成"修了按板子名加载签名密钥的配置问题"或干脆省略（用户不关心构建配置）。
  - **去掉一切代码/技术细节**：不得出现文件路径（如 `src/adc.c`）、变量名
    （如 `RF24_SLEEP_MS`）、配置项（如 `SB_CONFIG_BOOT_SIGNATURE_KEY_FILE`）、
    代码片段、commit SHA、反引号。
  - **省略对用户无感知的改动**：纯构建/打包/重构/文档类改动（如签名密钥路径调整、
    提交 changelog 本身）一般对固件行为无影响，可以**整条省略**，不要硬翻译成用户语言。
  - **去掉元信息行**：不要写"发布标签 / 发布提交 / 提交时间"三行——用户不需要 git
    概念。每个版本小节只保留版本号标题（含日期）和改动列表。
  - markdown 结构转纯文本的规则：一级标题 `# 手柄 变更记录` → 去掉 `#`，下加一行 `=`；
    二级标题 `## v1.0.1（2026-08-06）` → 去掉 `##`，下加一行 `-`；三级标题
    `### 自 ... 以来的改动` → **整个小节标题连同"发布标签/发布提交/提交时间"三行
    一并删除**，直接进入改动列表；无序列表 `- xxx` → 保留 `- xxx`。
  - 务必用 UTF-8 编码写入。
  - 若某版本改写后**没有任何对用户可见的改动**（全是构建/文档类），改动列表写
    `（本版本无面向用户的功能改动）`，不要留空，也不要把技术细节塞进去。
- 解压后得到三个文件、不带任何目录层级：`full_output.hex`、`update.bin`、`版本历史.txt`。
- 可用 Python 标准库完成，无需额外依赖，例如：

  注意：**改动条目的改写是语义操作，必须由 agent 逐条完成**（agent 读 CHANGELOG.md
  里某版本的每条改动，判断它对用户是否可见、如何用用户语言表述，省略纯构建/文档类），
  不能靠正则自动完成。下面脚本只负责**结构裁剪**（删元信息行、删三级小节标题、把
  markdown 标题转纯文本分隔线）。agent 先把改写后的用户语言条目拼成下面的
  `history_md`（只含一级标题、各版本二级标题、`- 改动描述` 列表，**不含**三级小节标题
  和元信息三行），再交给脚本：

  ```python
  import zipfile
  from pathlib import Path

  def md_to_txt(md: str) -> str:
      """history_md 只含一级标题、版本二级标题、改动列表，无元信息行/三级小节标题。"""
      lines = md.splitlines()
      out = []
      for ln in lines:
          if ln.startswith("# "):
              out.append(ln[2:].strip())
              out.append("=" * max(len(out[-1]), 3))
          elif ln.startswith("## "):
              out.append("")
              out.append(ln[3:].strip())
              out.append("-" * max(len(out[-1]), 3))
          else:
              out.append(ln)   # 改动列表行已在 agent 侧改写为用户语言，原样保留
      return "\n".join(out).strip() + "\n"

  # agent 逐条改写后拼出的、面向用户的版本历史（markdown 结构）。
  # 例：
  #   # 手柄 变更记录
  #   ## v1.0.1（2026-08-06）
  #   - 首个正式版本：摇杆/按键/无线/OLED 显示全功能上线
  #   - 无线模块采样间隔由 60ms 缩短为 50ms
  history_md = """<agent 改写后的 markdown 版本历史>"""

  images = Path("build/output/images")
  out = Path("<release_zip>")   # 如 "手柄-v1.0.1.zip"
  history_txt = md_to_txt(history_md)
  with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
      z.write(images / "full_output.hex", "full_output.hex")
      z.write(images / "app.bin", "update.bin")          # 改名为 update.bin
      z.writestr("版本历史.txt", history_txt.encode("utf-8"))
  ```

- 失败则报告并停下。

## 9. 汇报

用一段话给出 release 摘要：

- 应用：`<display_name>`（`<app_path>`）
- 版本：`v<version>`
- Tag：`<tag>`
- 板子：`<board>`，sysbuild：是/否
- 产物：
  - 归档 zip：`<board>_<app_name>.zip`（`west archive` 产出的完整镜像包，在 `build/` 下）
  - 发布镜像 zip：`<release_zip>`（含 `full_output.hex` + `update.bin` + `版本历史.txt`，在仓库根目录）

如果前面有步骤失败，则改为报告：哪些步骤成功了（例如"VERSION 已改写、已提交、已打
tag `<tag>`；编译失败"）、具体错误，以及建议的回滚命令
（`git tag -d <tag>` && `git reset --hard HEAD~1`）。

## 注意事项

- 本仓库两个 app 各自的 MCUboot 签名密钥独立（`<app_path>/boards/nrf24_f103rct6.pem`），
  镜像互不通用。
- 固件运行时通过 Zephyr 的 `<zephyr/app_version.h>`（`APP_VERSION_MAJOR/MINOR`、
  `APP_PATCHLEVEL`）读取版本号，该头文件在 configure 期由这个 `VERSION` 文件生成——
  所以改写 VERSION 文件就够了。
- 切勿提交 `*.pem`、`build/`、`*.log`。
