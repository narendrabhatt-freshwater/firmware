# Bash completion for fw
_fw_complete() {
  local cur prev words cword
  COMPREPLY=()
  cur="${COMP_WORDS[COMP_CWORD]}"
  prev="${COMP_WORDS[COMP_CWORD-1]}"

  local cmds="build flash list console log send cli play control clean status help"
  local cards_all="channel effect all"
  local cards_one="channel effect"
  local kinds="usb stlink uart all"
  local tools="cube dfu auto"
  local flash_flags="--debug --release --tool --port -p --sn --select --erase --no-verify --no-start"
  local serial_flags="--port -p --baud"

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
        COMPREPLY+=( $(compgen -W "$(ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/ttyACM* /dev/ttyUSB* 2>/dev/null)" -- "${cur}") )
      else
        COMPREPLY=( $(compgen -W "$(ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/ttyACM* /dev/ttyUSB* 2>/dev/null)" -- "${cur}") )
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
        COMPREPLY=( $(compgen -W "--debug --release --force -f" -- "${cur}") )
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
    status|info)
      COMPREPLY=( $(compgen -W "--debug --release" -- "${cur}") )
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
    control|gui)
      COMPREPLY=( $(compgen -W "run build" -- "${cur}") )
      ;;
    cli|play|perform|session)
      case "${prev}" in
        --midi) COMPREPLY=( $(compgen -W "auto off" -- "${cur}") ) ;;
        --attenuation) COMPREPLY=( $(compgen -W "0 6 12 24" -- "${cur}") ) ;;
        --baud) COMPREPLY=( $(compgen -W "921600" -- "${cur}") ) ;;
        --rs485|--cdc|--script|--sample|--samples|--wavetables|--audio)
          COMPREPLY=( $(compgen -f -- "${cur}") ) ;;
        *)
          COMPREPLY=( $(compgen -W "build --rs485 --cdc --script --sample --samples --wavetables --midi --audio --attenuation --baud --no-watch --list-midi --help" -- "${cur}") )
          COMPREPLY+=( $(compgen -d -- "${cur}") )
          ;;
      esac
      ;;
  esac
}

complete -F _fw_complete fw
