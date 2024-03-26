#!/usr/bin/bash

access_code_file=_access_code.txt

if [ -z "$RCI_ACCESS_CODE" ]; then
	RCI_ACCESS_CODE=$(cat $access_code_file 2> /dev/null)
fi

if [ "$2" = "r" ]; then
	echo "RG$RCI_ACCESS_CODE" | nc tejo.tecnico.ulisboa.pt 59011 > rep.html
	echo "Relatório gerado"
else
	if [ "$2" = "f" ] || [ ! -z "$RCI_ACCESS_CODE" ]; then
		echo "FIN$RCI_ACCESS_CODE" | nc tejo.tecnico.ulisboa.pt 59011
		echo
	fi
	if [ "$2" = "f" ]; then
		RCI_ACCESS_CODE=
	else
		echo "$1:$2" | nc tejo.tecnico.ulisboa.pt 59011 > init.html
		RCI_ACCESS_CODE="$(perl -n -e'/REPORT ACCESS CODE: (\d{7})/ && print $1' < init.html)"
		echo "Novo código de acesso: $RCI_ACCESS_CODE"
	fi
fi

echo -n $RCI_ACCESS_CODE > $access_code_file
