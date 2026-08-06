# Bash completion for fw
_fw_complete() {
  local cur prev words cword
  COMPREPLY=()
  cur="${COMP_WORDS[COMP_CWORD]}"
  prev="${COMP_WORDS[COMP_CWORD-1]}"

  local cmds="build flash list console log send rs485 midi clean status help"
  local cards_all="channel effect all"
  local cards_one="channel effect"
  local kinds="usb stlink uart all"
  local tools="cube dfu auto"
  local flash_flags="--debug --release --tool --port -p --sn --select --erase --no-verify --no-start"
  local serial_flags="--port -p --baud"
  local rs485_flags="--port -p --baud --manual-rts --timeout-ms --retries --echo-off"
  local rs485_subs="console build list send channel effect all"
  local midi_flags="--midi --port -p --rs485 --baud --gain --echo-off --echo-on --echo-leave --auto"
  local midi_subs="build list run play channel"

  if [[ ${COMP_CWORD} -eq 1 ]]; then
    COMPREPLY=( $(compgen -W "${cmds}" -- "${cur}") )
    return
  fi

  local cmd="${COMP_WORDS[1]}"

  case "${prev}" in
    --tool) COMPREPLY=( $(compgen -W "${tools}" -- "${cur}") ); return ;;
    --port|-p)
      if [[ "${cmd}" == flash ]]; then
        COMPREPLY=( $(compgen -W "USB1 USB2 USB3" -- "${cur}") )
        COMPREPLY+=( $(compgen -W "$(ls /dev/cu.usbmodem* /dev/ttyACM* 2>/dev/null)" -- "${cur}") )
      elif [[ "${cmd}" == rs485 ]]; then
        COMPREPLY=( $(compgen -W "$(ls /dev/cu.usbserial* /dev/tty.usbserial* /dev/ttyUSB* /dev/ttyACM* 2>/dev/null)" -- "${cur}") )
      else
        COMPREPLY=( $(compgen -W "$(ls /dev/cu.usbmodem* /dev/ttyACM* 2>/dev/null)" -- "${cur}") )
      fi
      return
      ;;
    --baud) COMPREPLY=( $(compgen -W "460800 115200 230400 9600" -- "${cur}") ); return ;;
    --timeout-ms) COMPREPLY=( $(compgen -W "300" -- "${cur}") ); return ;;
    --retries) COMPREPLY=( $(compgen -W "2" -- "${cur}") ); return ;;
  esac

  case "${cmd}" in
    build|clean)
      if [[ ${COMP_CWORD} -eq 2 ]]; then
        COMPREPLY=( $(compgen -W "${cards_all}" -- "${cur}") )
      else
        COMPREPLY=( $(compgen -W "--debug --release" -- "${cur}") )
      fi
      ;;
    flash)
      if [[ ${COMP_CWORD} -eq 2 ]]; then
        COMPREPLY=( $(compgen -W "${cards_one}" -- "${cur}") )
      else
        COMPREPLY=( $(compgen -W "${flash_flags}" -- "${cur}") )
      fi
      ;;
    list|ls|devices)
      COMPREPLY=( $(compgen -W "${kinds}" -- "${cur}") )
      ;;
    console|log|logs|follow|tail|shell|repl)
      if [[ ${COMP_CWORD} -eq 2 ]]; then
        COMPREPLY=( $(compgen -W "${cards_one}" -- "${cur}") )
      else
        COMPREPLY=( $(compgen -W "${serial_flags}" -- "${cur}") )
      fi
      ;;
    send|cmd)
      if [[ ${COMP_CWORD} -eq 2 ]]; then
        COMPREPLY=( $(compgen -W "${cards_one}" -- "${cur}") )
      elif [[ ${COMP_CWORD} -eq 3 ]]; then
        COMPREPLY=( $(compgen -W "help status sw led" -- "${cur}") )
      else
        COMPREPLY=( $(compgen -W "${serial_flags}" -- "${cur}") )
      fi
      ;;
    rs485)
      if [[ ${COMP_CWORD} -eq 2 ]]; then
        COMPREPLY=( $(compgen -W "${rs485_subs}" -- "${cur}") )
      elif [[ ${COMP_CWORD} -eq 3 && "${COMP_WORDS[2]}" == send ]]; then
        COMPREPLY=( $(compgen -W "${cards_all}" -- "${cur}") )
      elif [[ ${COMP_CWORD} -eq 4 && "${COMP_WORDS[2]}" == send ]]; then
        COMPREPLY=( $(compgen -W "help status sw led" -- "${cur}") )
      elif [[ ${COMP_CWORD} -eq 3 && "${COMP_WORDS[2]}" == console ]]; then
        COMPREPLY=( $(compgen -W "${cards_all}" -- "${cur}") )
      else
        COMPREPLY=( $(compgen -W "${rs485_flags}" -- "${cur}") )
      fi
      ;;
    midi)
      if [[ ${COMP_CWORD} -eq 2 ]]; then
        COMPREPLY=( $(compgen -W "${midi_subs} ${midi_flags}" -- "${cur}") )
      else
        COMPREPLY=( $(compgen -W "${midi_flags}" -- "${cur}") )
      fi
      ;;
  esac
}

complete -F _fw_complete fw
