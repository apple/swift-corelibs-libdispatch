#! /bin/bash -e

test "$ACTION" = install || exit 0

list_objc_syms ()
{
    nm -arch $1 -nU ${DSTROOT}/usr/lib/system/libdispatch.dylib | grep _OBJC | cut -d' ' -f3
}

list_mutable_data_syms ()
{
    nm -arch $1 -m ${DSTROOT}/usr/lib/system/libdispatch.dylib |grep __DATA|egrep -v '(__const|__crash_info)'|sed 's/^.* //'
}

list_objc_order ()
{
    grep '^_OBJC' "${SCRIPT_INPUT_FILE_0}"
}

list_dirty_order ()
{
    grep '^[^#]' "${SCRIPT_INPUT_FILE_1}"
}

list_clean_order ()
{
    grep '^[^#]' "${SCRIPT_INPUT_FILE_2}"
}

fail=

case "$PLATFORM_NAME" in
    *simulator) exit 0;;
    *) ;;
esac

if comm -12 <(list_dirty_order | sort) <(list_clean_order | sort) | grep .; then
    echo 1>&2 "error: *** SYMBOLS CAN'T BE BOTH CLEAN AND DIRTY ***"
    comm 1>&2 -12 <(list_dirty_order | sort) <(list_clean_order | sort)
    fail=t
fi

for arch in $ARCHS; do
    if test "$PLATFORM_NAME" = macosx -a "$arch" = i386; then
        continue
    fi

    if list_mutable_data_syms $arch | sort | uniq -c | grep -qvw 1; then
        echo 1>&2 "error: *** DUPLICATED SYMBOL NAMES FOR SLICE $arch ***"
        list_mutable_data_syms $arch | sort | uniq -c | grep -qw 1 1>&2
        fail=t
    fi

    if comm -23 <(list_mutable_data_syms $arch | sort) <((list_dirty_order; list_clean_order) | sort) | grep -q .; then
        echo 1>&2 "error: *** SYMBOLS NOT MARKED CLEAN OR DIRTY FOR SLICE $arch ***"
        comm 1>&2 -23 <(list_mutable_data_syms $arch | sort) <((list_dirty_order; list_clean_order) | sort)
        fail=t
    fi

    if comm -13 <(list_mutable_data_syms $arch | sort) <((list_dirty_order; list_clean_order) | sort) | grep -q .; then
        echo 1>&2 "warning: *** Found unknown symbols in dirty/clean files for slice $arch ***"
        comm 1>&2 -13 <(list_mutable_data_syms $arch | sort) <((list_dirty_order; list_clean_order) | sort)
    fi

    if ! cmp -s <(list_objc_syms $arch) <(list_objc_order); then
        echo 1>&2 "error: *** SYMBOL ORDER IS NOT WHAT IS EXPECTED FOR SLICE $arch ***"
        diff 1>&2 -U100 <(list_objc_syms $arch) <(list_objc_order) || fail=t
    fi
done

test -z "$fail"
