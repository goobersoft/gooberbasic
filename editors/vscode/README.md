# GooberBASIC for Visual Studio Code

Syntax highlighting, auto-indent and snippets for GooberBASIC (`.gb`) programs.

- Keywords, types (`int`, `t_enemy`, `list<float>`), built-in commands, `KEY_*` constants,
  numbers (`$FF`, `%1010`, `2.5e-3`), strings with `~n`-style escapes, `'` comments and
  `rem` … `endrem` blocks.
- Mistakes are flagged as you type: unknown escapes like `~x`, and removed keywords
  like `delete`.
- Lines indent after `if`, `for`, `while`, `repeat`, `select`, `function` and `type`,
  and the closing keyword moves back out.
- Snippets: `game` (window + main loop), `function`, `type`, `for`, `eachin`, `list`,
  `if`, `while`, `repeat`, `select`.

## Install

Try it without installing. This opens a VS Code window with the extension loaded:

```bash
code --extensionDevelopmentPath="$PWD/editors/vscode" examples
```

Or install it for good by linking it into your extensions folder, then restart VS Code:

```bash
ln -s "$PWD/editors/vscode" ~/.vscode/extensions/gooberbasic
```

Run both commands from the GooberBASIC folder.

## Run programs from VS Code

Add this as `.vscode/tasks.json` in your project. **Ctrl+Shift+B** then runs the open
file, and compile or runtime errors show up in the Problems panel with a link to the line:

```json
{
  "version": "2.0.0",
  "tasks": [
    {
      "label": "Run GooberBASIC",
      "type": "shell",
      "command": "/path/to/gooberbasic/goober",
      "args": ["${relativeFile}"],
      "group": { "kind": "build", "isDefault": true },
      "problemMatcher": {
        "owner": "gooberbasic",
        "fileLocation": ["relative", "${workspaceFolder}"],
        "pattern": {
          "regexp": "^(.*\\.gb):(\\d+):\\s+(?:runtime\\s+)?(error):\\s+(.*)$",
          "file": 1, "line": 2, "severity": 3, "message": 4
        }
      }
    }
  ]
}
```

Replace `/path/to/gooberbasic/goober` with the path to your `goober` binary.

## Keeping it up to date

The keyword, built-in command and constant lists in the grammar are generated from
the compiler's C sources. After adding a command or keyword, regenerate it:

```bash
make vscode
```

`generate.py` stops with an error if the lexer has a keyword the grammar doesn't know
how to colour, so a new keyword can't be left out by accident.
