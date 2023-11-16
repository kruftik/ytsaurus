GO_LIBRARY()

SRCS(
    alg.go
    arena.go
    asan0.go
    asm.s
    atomic_pointer.go
    cgo.go
    cgocall.go
    cgocallback.go
    cgocheck.go
    chan.go
    checkptr.go
    compiler.go
    complex.go
    covercounter.go
    covermeta.go
    cpuflags.go
    cpuprof.go
    debug.go
    debugcall.go
    debuglog.go
    debuglog_off.go
    env_posix.go
    error.go
    exithook.go
    extern.go
    fastlog2.go
    fastlog2table.go
    float.go
    hash64.go
    heapdump.go
    histogram.go
    iface.go
    lfstack.go
    lockrank.go
    lockrank_off.go
    malloc.go
    map.go
    map_fast32.go
    map_fast64.go
    map_faststr.go
    mbarrier.go
    mbitmap.go
    mcache.go
    mcentral.go
    mcheckmark.go
    mem.go
    metrics.go
    mfinal.go
    mfixalloc.go
    mgc.go
    mgclimit.go
    mgcmark.go
    mgcpacer.go
    mgcscavenge.go
    mgcstack.go
    mgcsweep.go
    mgcwork.go
    mheap.go
    minmax.go
    mpagealloc.go
    mpagealloc_64bit.go
    mpagecache.go
    mpallocbits.go
    mprof.go
    mranges.go
    msan0.go
    msize.go
    mspanset.go
    mstats.go
    mwbbuf.go
    netpoll.go
    os_nonopenbsd.go
    pagetrace_off.go
    panic.go
    pinner.go
    plugin.go
    preempt.go
    print.go
    proc.go
    profbuf.go
    proflabel.go
    rdebug.go
    runtime.go
    runtime1.go
    runtime2.go
    runtime_boring.go
    rwmutex.go
    select.go
    sema.go
    sigqueue.go
    sizeclasses.go
    slice.go
    softfloat64.go
    stack.go
    stkframe.go
    string.go
    stubs.go
    symtab.go
    symtabinl.go
    sys_nonppc64x.go
    tagptr.go
    tagptr_64bit.go
    time.go
    time_nofake.go
    trace.go
    traceback.go
    type.go
    typekind.go
    unsafe.go
    utf8.go
    write_err.go
)

GO_TEST_SRCS(
    align_runtime_test.go
    export_debuglog_test.go
    export_test.go
    importx_test.go
    proc_runtime_test.go
    symtabinl_test.go
    tracebackx_test.go
)

GO_XTEST_SRCS(
    abi_test.go
    align_test.go
    arena_test.go
    callers_test.go
    chan_test.go
    chanbarrier_test.go
    checkptr_test.go
    closure_test.go
    complex_test.go
    crash_cgo_test.go
    crash_test.go
    debuglog_test.go
    defer_test.go
    ehooks_test.go
    env_test.go
    example_test.go
    fastlog2_test.go
    float_test.go
    gc_test.go
    gcinfo_test.go
    hash_test.go
    heap_test.go
    histogram_test.go
    iface_test.go
    import_test.go
    lfstack_test.go
    lockrank_test.go
    malloc_test.go
    map_benchmark_test.go
    map_test.go
    memmove_test.go
    metrics_test.go
    mfinal_test.go
    mgclimit_test.go
    mgcpacer_test.go
    mgcscavenge_test.go
    minmax_test.go
    mpagealloc_test.go
    mpagecache_test.go
    mpallocbits_test.go
    mranges_test.go
    netpoll_os_test.go
    norace_test.go
    panic_test.go
    panicnil_test.go
    pinner_test.go
    proc_test.go
    profbuf_test.go
    rand_test.go
    runtime-gdb_test.go
    runtime-lldb_test.go
    runtime_test.go
    rwmutex_test.go
    sema_test.go
    sizeof_test.go
    slice_test.go
    softfloat64_test.go
    stack_test.go
    start_line_test.go
    string_test.go
    symtab_test.go
    time_test.go
    trace_cgo_test.go
    traceback_test.go
)

IF (ARCH_X86_64)
    SRCS(
        asm_amd64.s
        cpuflags_amd64.go
        cputicks.go
        duff_amd64.s
        memclr_amd64.s
        memmove_amd64.s
        preempt_amd64.s
        stubs_amd64.go
        sys_x86.go
        test_amd64.go
        test_amd64.s
    )

    GO_XTEST_SRCS(start_line_amd64_test.go)
ENDIF()

IF (ARCH_ARM64)
    SRCS(
        asm_arm64.s
        atomic_arm64.s
        cpuflags_arm64.go
        duff_arm64.s
        memclr_arm64.s
        memmove_arm64.s
        preempt_arm64.s
        stubs_arm64.go
        sys_arm64.go
        test_stubs.go
        tls_arm64.s
        tls_stub.go
    )
ENDIF()

IF (OS_LINUX)
    SRCS(
        cgo_mmap.go
        cgo_sigaction.go
        create_file_unix.go
        lock_futex.go
        mem_linux.go
        nbpipe_pipe2.go
        netpoll_epoll.go
        nonwindows_stub.go
        os_linux.go
        os_linux_generic.go
        os_unix.go
        preempt_nonwindows.go
        retry.go
        security_linux.go
        security_unix.go
        signal_unix.go
        sigqueue_note.go
        sigtab_linux_generic.go
        stubs2.go
        stubs3.go
        stubs_linux.go
        vdso_elf64.go
        vdso_linux.go
    )

    GO_TEST_SRCS(
        export_debug_test.go
        export_linux_test.go
        export_mmap_test.go
        export_pipe2_test.go
        export_unix_test.go
    )

    GO_XTEST_SRCS(
        crash_unix_test.go
        debug_test.go
        nbpipe_fcntl_unix_test.go
        nbpipe_test.go
        norace_linux_test.go
        runtime-gdb_unix_test.go
        runtime_linux_test.go
        runtime_mmap_test.go
        runtime_unix_test.go
        security_test.go
        semasleep_test.go
        syscall_unix_test.go
    )
ENDIF()

IF (OS_LINUX AND ARCH_X86_64)
    SRCS(
        defs_linux_amd64.go
        os_linux_noauxv.go
        os_linux_x86.go
        rt0_linux_amd64.s
        signal_amd64.go
        signal_linux_amd64.go
        sys_linux_amd64.s
        time_linux_amd64.s
        timeasm.go
        tls_stub.go
        vdso_linux_amd64.go
    )

    GO_TEST_SRCS(export_debug_amd64_test.go)

    GO_XTEST_SRCS(memmove_linux_amd64_test.go)
ENDIF()

IF (OS_LINUX AND ARCH_ARM64)
    SRCS(
        defs_linux_arm64.go
        os_linux_arm64.go
        rt0_linux_arm64.s
        signal_arm64.go
        signal_linux_arm64.go
        sys_linux_arm64.s
        timestub.go
        timestub2.go
        vdso_linux_arm64.go
    )

    GO_TEST_SRCS(export_debug_arm64_test.go)
ENDIF()

IF (OS_DARWIN)
    SRCS(
        create_file_unix.go
        lock_sema.go
        mem_darwin.go
        nbpipe_pipe.go
        netpoll_kqueue.go
        nonwindows_stub.go
        os_darwin.go
        os_unix.go
        os_unix_nonlinux.go
        preempt_nonwindows.go
        retry.go
        security_issetugid.go
        security_unix.go
        signal_darwin.go
        signal_unix.go
        stubs_nonlinux.go
        sys_darwin.go
        sys_libc.go
        timestub.go
        vdso_in_none.go
    )

    GO_TEST_SRCS(
        export_darwin_test.go
        export_mmap_test.go
        export_pipe_test.go
        export_unix_test.go
    )

    GO_XTEST_SRCS(
        crash_unix_test.go
        nbpipe_fcntl_libc_test.go
        nbpipe_pipe_test.go
        nbpipe_test.go
        runtime-gdb_unix_test.go
        runtime_mmap_test.go
        runtime_unix_test.go
        security_test.go
        semasleep_test.go
        syscall_unix_test.go
    )
ENDIF()

IF (OS_DARWIN AND ARCH_X86_64)
    SRCS(
        defs_darwin_amd64.go
        rt0_darwin_amd64.s
        signal_amd64.go
        signal_darwin_amd64.go
        sys_darwin_amd64.s
        tls_stub.go
    )
ENDIF()

IF (OS_DARWIN AND ARCH_ARM64)
    SRCS(
        defs_darwin_arm64.go
        os_darwin_arm64.go
        rt0_darwin_arm64.s
        signal_arm64.go
        signal_darwin_arm64.go
        sys_darwin_arm64.go
        sys_darwin_arm64.s
    )
ENDIF()

IF (OS_WINDOWS)
    SRCS(
        auxv_none.go
        create_file_nounix.go
        defs_windows.go
        lock_sema.go
        mem_windows.go
        netpoll_windows.go
        os_windows.go
        security_nonunix.go
        signal_windows.go
        sigqueue_note.go
        stubs3.go
        stubs_nonlinux.go
        syscall_windows.go
        timeasm.go
        vdso_in_none.go
        zcallback_windows.go
    )

    GO_TEST_SRCS(export_windows_test.go)

    GO_XTEST_SRCS(
        runtime-seh_windows_test.go
        signal_windows_test.go
        syscall_windows_test.go
    )
ENDIF()

IF (OS_WINDOWS AND ARCH_X86_64)
    SRCS(
        defs_windows_amd64.go
        rt0_windows_amd64.s
        sys_windows_amd64.s
        time_windows_amd64.s
        tls_windows_amd64.go
        zcallback_windows.s
    )
ENDIF()

IF (OS_WINDOWS AND ARCH_ARM64)
    SRCS(
        defs_windows_arm64.go
        os_windows_arm64.go
        rt0_windows_arm64.s
        sys_windows_arm64.s
        time_windows_arm64.s
        zcallback_windows_arm64.s
    )
ENDIF()

IF (CGO_ENABLED OR OS_DARWIN)
    IF (RACE)
        SRCS(
            race.go
        )

        IF (ARCH_ARM64)
            SRCS(
                race_arm64.s
            )
        ENDIF()

        IF (ARCH_X86_64)
            SRCS(
                race_amd64.s
            )
        ENDIF()
    ELSE()
        SRCS(
            race0.go
        )
    ENDIF()
ELSE()
    SRCS(
        race0.go
    )
ENDIF()


END()

RECURSE(
    coverage
    debug
    internal
    metrics
    pprof
    race
    trace
)

IF (CGO_ENABLED)
    RECURSE(
        cgo
    )
ENDIF()
