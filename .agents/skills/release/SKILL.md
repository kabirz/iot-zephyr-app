---
name: release
description: Use when the user asks to release/publish a firmware version for an app in this Zephyr repo (e.g. "release angle-handler to 0.1.5", "发布 n2e-gw 0.2.0", "给 io-edge-hub 发版 0.1.0"). 改写 app 的 VERSION 文件、提交、打 tag，然后用指定的 Python 虚拟环境编译并归档产物。需要用户提供：项目名称、版本号、python虚拟环境使能方法。仅用于正式发版，不要在普通编译/调试场景加载本 skill。注意：会有两次提交（VERSION 文件提交和 changelog/版本历史提交），tag 指向 VERSION 提交。
---

# 发布固件版本

本 skill 为本 Zephyr west manifest 仓库里的某个 app 切出一个正式版本：改写 app 的 `VERSION` 文件、提交、
**先打 tag 再编译**、用用户指定的 Python 虚拟环境编译、最后归档镜像。必须严格按
顺序执行每一步；任何一步失败都要立即停下，并报告已经成功的步骤，方便用户回滚。

**注意**：本流程包含两次提交：
1. VERSION 文件提交（tag 指向此 commit）
2. changelog/版本历史提交（引用已打的 tag）

## 1. 收集参数

从用户消息中提取以下参数。若任何一个必填项缺失或含糊，先询问用户，不要动手。

- **`<app_name>`**（必填）— app 名称，如 `angle-handler`、`n2e-gw`、`io-edge-hub`。
  必须存在且包含 `VERSION` 文件。
- **`<version>`**（必填）— 目标版本号，`MAJOR.MINOR.PATCH` 格式，如 `0.1.5`。不匹配
  `^\d+\.\d+\.\d+$` 的要拒绝并重新询问。
- **`<venv_activate>`**（必填）— Python 虚拟环境使能方法，如 `.venv/bin/activate`、
  `C:\Users\user\.venv\Scripts\activate.ps1` 等。用户需要提供完整的使能命令。

派生量：
- **`<app_path>`** = `applications/<app_name>`（如 `applications/io-edge-hub`）。
- **`<display_name>`** = app 的中文显示名（用于 changelog 标题、发布 zip 命名、汇报）。
  固定映射：`angle-handler` → `手柄`，`n2e-gw` → `手柄接收器`，`io-edge-hub` → `数据采集卡`。
  其他 app 报错并询问用户。
- **`<board>`** = 目标板子：`io-edge-hub` → `io_edge_f407vet6`，`angle-handler` / `n2e-gw` → `nrf24_f103rct6`。
- **`<tag>`** = `v<version>-<app_name>`（如 `v1.0.1-angle-handler`，**tag 用英文 app_name，
  不用中文**，避免中文在 git tag 里出问题）。
- **`<prev_tag>`** = 该 app 上一个 release tag。用
  `git tag --list 'v*-<app_name>' --sort=-v:refname` 取出该 app 所有历史 release tag，
  按版本号倒序，第一个就是上一个 release（本次的 `<tag>` 此时还没打，不会出现在列表里）。
  若列表为空，说明是首次发版。

## 2. 预检（全部通过才继续；任一失败立即中止，且不改动任何文件）

在修改任何文件之前先跑这些检查，遇到第一个失败就停下并报告。

1. **工作区干净**：没有未提交改动。
   `git status --porcelain` 输出必须为空。
2. **app 存在**：`<app_path>` 是目录且含有 `VERSION` 文件。
3. **tag 未被占用**：`<tag>` 当前不存在。
   `git rev-parse <tag>` 必须失败（返回非零）。
4. **python 可用**：使用用户指定的虚拟环境使能方法，验证 python 可用。

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

**注意**：这是第一次提交（VERSION 文件提交），tag 指向这个 commit。changelog 和版本历史
的提交将在后续步骤中进行，它们会引用这个 tag。

若提交或打 tag 失败，停下并报告（VERSION 的改动已暂存/已提交，但没打成 tag；用户
可能想 `git reset`）。

## 5. 编译

使用用户提供的虚拟环境使能方法，激活 python 环境后进行编译。

**各项目编译命令**：

- **angle-handler**:
  ```
  west build -b nrf24_f103rct6 applications/angle-handler -d build/angle-handler --sysbuild
  ```

- **n2e-gw**:
  ```
  west build -b nrf24_f103rct6 applications/n2e-gw -d build/n2e-gw --sysbuild
  ```

- **io-edge-hub**:
  ```
  west build -b io_edge_f407vet6 applications/io-edge-hub -d build/io-edge-hub --sysbuild "-Dmcuboot_EXTRA_CONF_FILE=$PWD/applications/io-edge-hub/sysbuild/mcuboot.conf;$PWD/libs/can_fw_upgrade/mcuboot_can.conf"
  ```

编译失败则停下并报告——此时 tag 已经存在，要把 tag 名告诉用户，方便其修复后重跑编译，或删除 tag。

## 6. 更新 CHANGELOG.md 和生成版本历史.txt

编译成功后，执行以下步骤。这是第二次提交，changelog 和版本历史中将引用已在步骤4中打好的 tag。

### 6.1 确定改动范围

- 若存在 `<prev_tag>`：改动区间是 `git log <prev_tag>..<tag> -- <app_path>`。
- 若是首次发版（无 `<prev_tag>`）：从仓库根 commit 起算，列出截至本次 release 的全部本 app 提交。

### 6.2 更新 CHANGELOG.md

在 `<app_path>/CHANGELOG.md` 里追加一条记录。该文件按时间倒序排列（最新版本在最上面）。
**若文件不存在则创建**（首行写一级标题 `# <display_name> 变更记录`，如 `# 数据采集卡 变更记录`，
空一行后再写第一条记录）。

**每条改动必须用中文详细描述具体内容**，不能只抄 commit message 首行。对每个提交：

1. 先取该提交的元信息：`git show -s --format='%h %s' <commit>`。
2. 再取该提交**对本 app 目录**的具体改动：`git show --stat <commit> -- <app_path>`
   看改了哪些文件；必要时用 `git show <commit> -- <文件>` 看 diff 摘要。
3. 用一句通顺的中文概括这个提交**实际做了什么改动**（面向读 changelog 的用户，而非
   开发者）。

**记录格式**（追加到文件顶部、标题之下）：

```markdown
## v<version>（<提交日期>）

- 发布标签：`<tag>`
- 发布提交：`<6位短SHA>`
- 提交时间：`<YYYY-MM-DD HH:MM:SS +时区>`

### 自 <prev_tag 或 "仓库初始提交"> 以来的改动

- `<6位短SHA>` <中文详细描述>（`<改动的文件>`）
- `<6位短SHA>` <中文详细描述>（`<改动的文件>`）
- ...
```

### 6.2 更新 版本历史.txt

**版本历史.txt 不写任何代码的改动，只提硬件或功能的改动**。

在 `<app_path>/VERSION_HISTORY.txt` 里追加一条记录。该文件按时间倒序排列（最新版本在最上面）。
**若文件不存在则创建**（首行写一级标题 `# <display_name> 版本历史`，空一行后再写第一条记录）。
内容格式：

```
<display_name> 版本历史

v<version>（<提交日期>）

主要改动：
- <硬件/功能改动1>
- <硬件/功能改动2>
- ...

---
```
**CHANGELOG.md 和 VERSION_HISTORY.txt提交**（这是第二次提交，tag 已经存在）：

```
git add <app_path>/CHANGELOG.md <app_path>/VERSION_HISTORY.txt
git commit -m "docs: <app_name> changelog for v<version>"
```

## 7. 发布镜像

**注意**：`<commit_id>` 应使用步骤4中 VERSION 提交的短 SHA（即 tag 指向的 commit），而不是步骤6中 changelog/版本历史提交的 SHA。

- **angle-handler**:
```
west archive --no-rebuild -d build/angle-handler -o <display_name>-v<version>_<commit_id>.zip
```

- **n2e-gw**:
```
west archive --no-rebuild -d build/n2e-gw -o <display_name>-v<version>_<commit_id>.zip
```

- **io-edge-hub**:
```
west archive --no-rebuild -d build/io-edge-hub -o <display_name>-v<version>_<commit_id>.zip
```

## 8. 汇报

用一段话给出 release 摘要：

- 应用：`<display_name>`（`<app_path>`）
- 版本：`v<version>`
- Tag：`<tag>`
- 板子：`<board>`
- 提交：
  - VERSION 提交：`<VERSION_commit_sha>`（tag 指向此 commit）
  - changelog/版本历史提交：`<changelog_commit_sha>`
- 产物：
  - 发布镜像 zip：`<display_name>-v<version>_<commit_id>.zip`（含 `full_output.hex` + `update.bin` + `版本历史.txt`，在仓库根目录）

**注意**：`<commit_id>` 使用 VERSION 提交的短 SHA（`<VERSION_commit_sha>`）。

如果前面有步骤失败，则改为报告：哪些步骤成功了（例如"VERSION 已改写、已提交、已打
tag `<tag>`；编译失败"）、具体错误，以及建议的回滚命令
（`git tag -d <tag>` && `git reset --hard HEAD~1`）。

## 注意事项

- 本仓库各 app 各自的 MCUboot 签名密钥独立（`<app_path>/boards/${BOARD}.pem`），
  镜像互不通用。
- 固件运行时通过 Zephyr 的 `<zephyr/app_version.h>`（`APP_VERSION_MAJOR/MINOR`、
  `APP_PATCHLEVEL`）读取版本号，该头文件在 configure 期由这个 `VERSION` 文件生成——
  所以改写 VERSION 文件就够了。
- 切勿提交 `*.pem`、`build/`、`*.log`。
