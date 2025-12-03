#!/bin/ksh93

typeset -r VERSION='1.0' FPROG=${.sh.file} PROG=${FPROG##*/} SDIR=${FPROG%/*}
export LC_TIME=de_DE.UTF-8

function showUsage {
	[[ -n $1 ]] && X='-?' ||  X='--man'
	getopts -a ${PROG} "${ print ${USAGE} ; }" OPT $X
}


typeset -A IP NAME
unset C Z; integer C=1 Z
typeset -rla INCOG=( 'Hans' 'Wurst' 'Karl' 'Ranseier' 'Alexander' 'Platz'
	'Andreas' 'Kreuz' 'Anna' 'Nass' 'Marie' 'Huana' 'Claire' 'Grube'
	'Ellen' 'Lang' 'Frank' 'Reich' 'Herbert' 'Root' 'Perry' 'Ode'
	'Peter' 'Silie' 'Rainer' 'Zufall' 'Rob' 'Otter' 'Ron' 'Dell' 'Tim' 'Buktu'
	'Wilma' 'Ruhe'
	)
(( Z = RANDOM % ${#INCOG[@]} ))

function replaceIp {
	typeset -n I=$1
	integer N
	typeset T

	[[ $I == {1,3}([0-9]){3}(.{1,3}([0-9])) ]] || return
	T=${IP[${I}]}
	[[ -n $T ]] && I="$T" && return

	(( C == 0 )) && C=1
	for (( N=0; N < ${#I}; N++ )); do
		[[ ${I:N:1} != [0-9] ]] && T+="${I:N:1}" && continue
		T+="$C"
		(( C++ ))
		(( C == 10 )) && C=0
	done
	IP["$I"]="$T"
	I="$T"
}

function replaceUser {
	typeset -n U=$1

	typeset T=${NAME[${U}]}
	[[ -n $T ]] && U="$T" && return

	(( Z >= ${#INCOG[@]} )) && Z=0
	T=${INCOG[$Z]}'                                                       '
	NAME["$U"]="${T:0:${#U}}"
	(( Z++ ))
	U="${NAME[$U]}"
}

function doMain {
	while read L ; do
		[[ -z $L ]] && print && continue
		set $L
		U=$1
		shift
		T=$1
		shift
		[[ ${U:0:4} == 'wtmp' && $T == 'begins' ]] && print "$L" && break
		[[ ${U: -6:6} == 'reboot' ]] && { T+=" $1"; shift; } || replaceUser U
		I=$1
		replaceIp I
		shift
		PFX=${ printf '%-8s %-12s %-16s' "$U" "$T" "$I"; }
		print "${PFX}${L:${#PFX}}"
	done
}

USAGE="[-?${VERSION}"' ]
[-copyright?Copyright (c) 2025 Jens Elkner. All rights reserved.]
[-license?CDDL 1.0]
[+NAME?'"${PROG}"' - anonymizes output of the last command]
[+DESCRIPTION?This script processes the output of the last command and anonymizes specific user-related information to enhance privacy. It replaces usernames and host information with generic placeholders while maintaining the structural integrity of the original output. It expects in the first column the username, in the 2nd column the tty name, and 3rd column the remote host. Everything else gets copied as is.]
[+EXAMPLES?last | '"${PROG}"]'
[h:help?Print this help and exit.]
[F:functions?Print a list of all functions available.]
[T:trace]:[functionList?A comma separated list of functions of this script to trace (convinience for troubleshooting).] 
[+?]
[i:index]:[num?Use the given number as starting index for the username list to use (in case you need a stable output). Default: random]
'
X="${ print ${USAGE} ; }"
while getopts "${X}" OPT ; do
	case ${OPT} in
		h) showUsage ; exit 0 ;;
		T)	if [[ ${OPTARG} == 'ALL' ]]; then
				typeset -ft ${ typeset +f ; }
			else
				typeset -ft ${OPTARG//,/ }
			fi
			;;
		F) typeset +f && exit 0 ;;
		i) [[ ${OPTARG} == +([0-9]) ]] && (( Z = ${OPTARG} % ${#INCOG[@]} ));;
		*) showUsage 1 ; exit 1 ;;
	esac
done

X=$((OPTIND-1))
shift $X && OPTIND=1
unset X

doMain "$@"
