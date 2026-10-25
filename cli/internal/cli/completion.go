// SPDX-License-Identifier: GPL-3.0-only
package cli

import (
	"fmt"
	"io"
)

// Shell completion: `sshrunner completion <shell>` prints a script for that shell. It completes
// the commands, the flags, backup IDs (from `sshrunner backups ids`) and file paths.

var completionCommands = "status hosts pull push import-key backup restore backups completion version"
var completionBackups = "list export import delete"
var completionFlags = "--port --dir --backup-dir --keep-backups --yes --include-keys --no-keys --delete --name --json --no-mouse --help --version"

const bashCompletion = `# sshrunner completion for bash: source it from ~/.bashrc
_sshrunner() {
    local cur prev words cword
    _init_completion 2>/dev/null || { cur=${COMP_WORDS[COMP_CWORD]}; prev=${COMP_WORDS[COMP_CWORD-1]}; words=("${COMP_WORDS[@]}"); cword=$COMP_CWORD; }
    case "$prev" in
        --dir|--backup-dir) COMPREPLY=($(compgen -d -- "$cur")); return ;;
        --port|--keep-backups|--name) return ;;
    esac
    if [[ "$cur" == -* ]]; then COMPREPLY=($(compgen -W "%[3]s" -- "$cur")); return; fi
    local cmd="" sub="" i
    for ((i = 1; i < cword; i++)); do
        case "${words[i]}" in -*) ;; *) if [ -z "$cmd" ]; then cmd=${words[i]}; elif [ -z "$sub" ]; then sub=${words[i]}; fi ;; esac
    done
    case "$cmd" in
        "") COMPREPLY=($(compgen -W "%[1]s" -- "$cur")) ;;
        completion) COMPREPLY=($(compgen -W "bash zsh fish powershell" -- "$cur")) ;;
        backups)
            case "$sub" in
                "") COMPREPLY=($(compgen -W "%[2]s" -- "$cur")) ;;
                export|delete) COMPREPLY=($(compgen -W "$(sshrunner backups ids 2>/dev/null)" -- "$cur")) ;;
                *) COMPREPLY=($(compgen -f -- "$cur")) ;;
            esac ;;
        restore) COMPREPLY=($(compgen -W "$(sshrunner backups ids 2>/dev/null)" -- "$cur") $(compgen -d -- "$cur")) ;;
        pull|push) COMPREPLY=($(compgen -d -- "$cur")) ;;
        *) COMPREPLY=($(compgen -f -- "$cur")) ;;
    esac
}
complete -o filenames -F _sshrunner sshrunner
`

const zshCompletion = `#compdef sshrunner
# sshrunner completion for zsh: save as _sshrunner in a folder on $fpath, then compinit
_sshrunner() {
    local -a cmds subs
    cmds=(%[1]s)
    subs=(%[2]s)
    if [[ $words[CURRENT] == -* ]]; then compadd -- %[3]s; return; fi
    case $words[CURRENT-1] in
        --dir|--backup-dir) _directories; return ;;
    esac
    local -a args; args=(${words[2,CURRENT-1]:#-*})
    case $args[1] in
        "") compadd -a cmds ;;
        completion) compadd bash zsh fish powershell ;;
        backups)
            case $args[2] in
                "") compadd -a subs ;;
                export|delete) [[ ${#args} -eq 2 ]] && compadd -- ${(f)"$(sshrunner backups ids 2>/dev/null)"} || _files ;;
                *) _files ;;
            esac ;;
        restore) compadd -- ${(f)"$(sshrunner backups ids 2>/dev/null)"}; _directories ;;
        pull|push) _directories ;;
        *) _files ;;
    esac
}
compdef _sshrunner sshrunner
`

const fishCompletion = `# sshrunner completion for fish: save as ~/.config/fish/completions/sshrunner.fish
complete -c sshrunner -f
complete -c sshrunner -n "not __fish_seen_subcommand_from %[1]s" -a "%[1]s"
complete -c sshrunner -n "__fish_seen_subcommand_from completion" -a "bash zsh fish powershell"
complete -c sshrunner -n "__fish_seen_subcommand_from backups; and not __fish_seen_subcommand_from %[2]s" -a "%[2]s"
complete -c sshrunner -n "__fish_seen_subcommand_from export delete restore" -a "(sshrunner backups ids 2>/dev/null)"
complete -c sshrunner -n "__fish_seen_subcommand_from import import-key pull push" -F
complete -c sshrunner -l port -r
complete -c sshrunner -l dir -r -a "(__fish_complete_directories)"
complete -c sshrunner -l backup-dir -r -a "(__fish_complete_directories)"
complete -c sshrunner -l keep-backups -r
complete -c sshrunner -l name -r
complete -c sshrunner -l yes
complete -c sshrunner -l include-keys
complete -c sshrunner -l no-keys
complete -c sshrunner -l delete
complete -c sshrunner -l json
complete -c sshrunner -l no-mouse
complete -c sshrunner -l help
complete -c sshrunner -l version
`

const powershellCompletion = `# sshrunner completion for PowerShell: add this to your $PROFILE
Register-ArgumentCompleter -Native -CommandName sshrunner, sshrunner.exe -ScriptBlock {
    param($wordToComplete, $commandAst, $cursorPosition)
    $words = @($commandAst.CommandElements | Select-Object -Skip 1 | ForEach-Object { $_.ToString() } |
        Where-Object { $_ -notlike '-*' -and $_ -ne $wordToComplete })
    $values = @()
    if ($wordToComplete -like '-*') {
        $values = '%[3]s' -split ' '
    } elseif ($words.Count -eq 0) {
        $values = '%[1]s' -split ' '
    } elseif ($words[0] -eq 'completion') {
        $values = 'bash', 'zsh', 'fish', 'powershell'
    } elseif ($words[0] -eq 'backups' -and $words.Count -eq 1) {
        $values = '%[2]s' -split ' '
    } elseif (($words[0] -eq 'backups' -and $words[1] -in 'export', 'delete' -and $words.Count -eq 2) -or $words[0] -eq 'restore') {
        $values = @(& sshrunner backups ids 2>$null)
    }
    if ($values.Count -gt 0) {
        $values | Where-Object { $_ -like "$wordToComplete*" } |
            ForEach-Object { [System.Management.Automation.CompletionResult]::new($_, $_, 'ParameterValue', $_) }
    } else {
        Get-ChildItem -Path "$wordToComplete*" -ErrorAction SilentlyContinue |
            ForEach-Object { [System.Management.Automation.CompletionResult]::new($_.FullName, $_.Name, 'ProviderItem', $_.FullName) }
    }
}
`

func completion(out io.Writer, shell string) error {
	scripts := map[string]string{"bash": bashCompletion, "zsh": zshCompletion, "fish": fishCompletion, "powershell": powershellCompletion}
	s, ok := scripts[shell]
	if !ok {
		return fmt.Errorf("completion: one of bash, zsh, fish, powershell")
	}
	fmt.Fprintf(out, s, completionCommands, completionBackups, completionFlags)
	return nil
}
