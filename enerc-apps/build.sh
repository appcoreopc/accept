util=$here/../util.sh
if [ "$1" = "" ] ; then
    eval $util build $args
    eval $util analyze $args
    eval $util profile $args
    eval $util danalyze $args
else
    eval $util $1 $args
fi
