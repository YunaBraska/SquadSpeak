# Pinned media transport; signaling and admission remain in LocalChannel.
function(squad_add_media)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_TESTING OFF)
    set(NO_TESTS ON)
    set(NO_EXAMPLES ON)
    set(NO_WEBSOCKET ON)
    set(NO_MEDIA OFF)
    set(LIBSRTP_TEST_APPS OFF)
    set(ENABLE_WARNINGS_AS_ERRORS OFF)
    set(USE_NICE OFF)
    set(PREFER_SYSTEM_LIB OFF)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/cmake/source_archives.json" sources)
    string(JSON count LENGTH "${sources}" archives)
    math(EXPR last "${count} - 1")
    foreach(index RANGE ${last})
        string(JSON archive GET "${sources}" archives ${index})
        string(JSON name GET "${archive}" name)
        if(NOT name MATCHES "^(libdatachannel|libjuice|libsrtp|plog|usrsctp)-")
            continue()
        endif()
        set(dependency "${CMAKE_MATCH_1}")
        string(JSON url GET "${archive}" url)
        string(JSON hash GET "${archive}" sha256)
        if(dependency STREQUAL "libdatachannel")
            set(directory "${CMAKE_BINARY_DIR}/_deps/media-src")
        else()
            set(directory "${CMAKE_BINARY_DIR}/_deps/media-src/deps/${dependency}")
        endif()
        FetchContent_Declare(squad_${dependency} URL "${url}" URL_HASH "SHA256=${hash}"
            SOURCE_DIR "${directory}" SOURCE_SUBDIR unused DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(squad_${dependency})
    endforeach()
    # Both parsers must accept numeric endpoints such as ::1 even without a
    # non-loopback IPv6 interface. Hostname lookup retains AI_ADDRCONFIG.
    foreach(resolver candidate.cpp deps/libjuice/src/ice.c)
        if(resolver STREQUAL "candidate.cpp")
            set(path "${CMAKE_BINARY_DIR}/_deps/media-src/src/${resolver}")
            set(numeric_mode "mode == ResolveMode::Simple")
        else()
            set(path "${CMAKE_BINARY_DIR}/_deps/media-src/${resolver}")
            set(numeric_mode "mode != ICE_RESOLVE_MODE_LOOKUP")
        endif()
        file(READ "${path}" code)
        set(before "\thints.ai_flags = AI_ADDRCONFIG;\n")
        set(after "\thints.ai_flags = ${numeric_mode} ? AI_NUMERICHOST : AI_ADDRCONFIG;\n")
        string(FIND "${code}" "${before}" position)
        if(position GREATER -1)
            string(REPLACE "${before}" "${after}" code "${code}")
            file(WRITE "${path}" "${code}")
        else()
            string(FIND "${code}" "${after}" position)
            if(position EQUAL -1)
                message(FATAL_ERROR "Unexpected pinned candidate resolver: ${resolver}")
            endif()
        endif()
    endforeach()
    # Match the generic hash callback signatures; casting typed functions to
    # void pointers is undefined behavior and fails Clang function sanitization.
    set(path "${CMAKE_BINARY_DIR}/_deps/media-src/deps/libjuice/src/picohash.h")
    file(READ "${path}" original)
    set(patched "${original}")
    foreach(algorithm md5 sha1 sha224 sha256 hmac)
        set(context "_picohash_${algorithm}_ctx_t")
        if(algorithm STREQUAL "sha224")
            set(context "_picohash_sha256_ctx_t")
        elseif(algorithm STREQUAL "hmac")
            set(context "picohash_ctx_t")
        endif()
        set(function "(_picohash_${algorithm}_(init|update|final|reset))")
        string(REGEX REPLACE "${function}\\(${context} \\*(ctx|s)([^\n]*)\\)\n\\{"
            "\\1(void *opaque\\4)\n{\n    ${context} *\\3 = opaque;" patched "${patched}")
        string(REGEX REPLACE "${function}\\(${context} \\*(ctx|s)"
            "\\1(void *opaque" patched "${patched}")
    endforeach()
    string(REPLACE "(void *)_picohash_" "_picohash_" patched "${patched}")
    if(NOT patched STREQUAL original)
        file(WRITE "${path}" "${patched}")
    endif()
    # Pending ClientHello packets can run on another worker as soon as receive
    # is registered. Initialize the DTLS MTU before allowing that worker in.
    set(path "${CMAKE_BINARY_DIR}/_deps/media-src/src/impl/dtlstransport.cpp")
    file(READ "${path}" code)
    set(before "\tregisterIncoming();\n\tchangeState(State::Connecting);\n\n\tint ret, err;")
    set(after "\tint ret, err;")
    set(handshake "\t\t// Initiate the handshake\n")
    set(ready "\t\tregisterIncoming();\n\t\tchangeState(State::Connecting);\n\n${handshake}")
    string(FIND "${code}" "${before}" position)
    if(position GREATER -1)
        string(FIND "${code}" "${handshake}" handshake_position)
        if(handshake_position EQUAL -1)
            message(FATAL_ERROR "Unexpected pinned DTLS handshake initialization")
        endif()
        string(REPLACE "${before}" "${after}" code "${code}")
        string(REPLACE "${handshake}" "${ready}" code "${code}")
        file(WRITE "${path}" "${code}")
    else()
        string(FIND "${code}" "${ready}" position)
        if(position EQUAL -1)
            message(FATAL_ERROR "Unexpected pinned DTLS receive registration")
        endif()
    endif()
    # SCTP copies a send outside the association lock. If the peer aborts
    # meanwhile, the new message is not queued and must be freed by the sender.
    set(path "${CMAKE_BINARY_DIR}/_deps/media-src/deps/usrsctp/usrsctplib/netinet/sctp_output.c")
    file(READ "${path}" code)
    set(before [=[			sp = sctp_copy_it_in(stcb, asoc, sndrcvninfo, uio, net, max_len, user_marks_eor, &error);
			SCTP_TCB_LOCK(stcb);
			if ((asoc->state & SCTP_STATE_ABOUT_TO_BE_FREED) ||
			    (asoc->state & SCTP_STATE_WAS_ABORTED)) {
]=])
    set(cleanup [=[				if (sp != NULL) {
					if (sp->data != NULL) {
						sctp_m_freem(sp->data);
						sp->data = sp->tail_mbuf = NULL;
						sp->length = 0;
					}
					if (sp->net != NULL) {
						sctp_free_remote_addr(sp->net);
						sp->net = NULL;
					}
					sctp_free_a_strmoq(stcb, sp, SCTP_SO_LOCKED);
				}
]=])
    set(after "${before}${cleanup}")
    string(FIND "${code}" "${after}" position)
    if(position EQUAL -1)
        string(FIND "${code}" "${before}" position)
        if(position EQUAL -1)
            message(FATAL_ERROR "Unexpected pinned SCTP send cleanup")
        endif()
        string(REPLACE "${before}" "${after}" code "${code}")
        file(WRITE "${path}" "${code}")
    endif()
    add_subdirectory("${CMAKE_BINARY_DIR}/_deps/media-src" "${CMAKE_BINARY_DIR}/_deps/media-build" EXCLUDE_FROM_ALL)
endfunction()
squad_add_media()
