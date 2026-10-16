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
    # A reconnect can reuse an endpoint before the previous ICE agent is
    # destroyed. STUN credentials select the new agent, not the address cache.
    set(path "${CMAKE_BINARY_DIR}/_deps/media-src/deps/libjuice/src/conn_mux.c")
    file(READ "${path}" original)
    set(code "${original}")
    set(before [=[	if (agent) {
		JLOG_DEBUG("Found agent from address");
		return agent;
	}

	if (!is_stun_datagram(buf, len)) {
		JLOG_INFO("Got non-STUN message from unknown source address");
		return NULL;
	}
]=])
    set(after [=[	if (!is_stun_datagram(buf, len)) {
		if (agent)
			JLOG_DEBUG("Found agent from address");
		else
			JLOG_INFO("Got non-STUN message from unknown source address");
		return agent;
	}
]=])
    string(FIND "${code}" "${before}" position)
    if(position GREATER -1)
        string(REPLACE "${before}" "${after}" code "${code}")
    else()
        string(FIND "${code}" "${after}" position)
        if(position EQUAL -1)
            message(FATAL_ERROR "Unexpected pinned ICE address lookup")
        endif()
    endif()
    set(before [=[					JLOG_DEBUG("Found agent from ICE ufrag");
					insert_map_entry(registry_impl, src, agent);
]=])
    set(after [=[					JLOG_DEBUG("Found agent from ICE ufrag");
					if (agent_verify_stun_binding(agent, buf, len, &msg) != 0)
						return NULL;
					insert_map_entry(registry_impl, src, agent);
]=])
    string(FIND "${code}" "${after}" position)
    if(position EQUAL -1)
        string(FIND "${code}" "${before}" position)
        if(position EQUAL -1)
            message(FATAL_ERROR "Unexpected pinned ICE binding lookup")
        endif()
        string(REPLACE "${before}" "${after}" code "${code}")
    endif()
    # Indications have no transaction to look up and retain their existing route.
    string(REPLACE [=[			JLOG_INFO("Got unexpected STUN message from unknown source address");
			return NULL;]=] [=[			if (!agent)
				JLOG_INFO("Got unexpected STUN message from unknown source address");
			return agent;]=] code "${code}")
    if(NOT code STREQUAL original)
        file(WRITE "${path}" "${code}")
    endif()
    # The shared socket keeps its address family for its entire lifetime. Avoid
    # querying it again for every packet in a many-listener audio relay.
    foreach(source udp.h udp.c conn_mux.c conn_poll.c conn_thread.c server.c)
        set(path "${CMAKE_BINARY_DIR}/_deps/media-src/deps/libjuice/src/${source}")
        file(READ "${path}" original)
        set(code "${original}")
        if(source MATCHES "^udp\\.[ch]$")
            string(REPLACE
                "int udp_sendto(socket_t sock, const char *data, size_t size, const addr_record_t *dst)"
                "int udp_sendto(socket_t sock, const char *data, size_t size, const addr_record_t *dst, int family)"
                code "${code}")
            if(source STREQUAL "udp.c")
                set(before [=[	addr_record_t name;
	name.len = sizeof(name.addr);
	name.socktype = SOCK_DGRAM;
	if (getsockname(sock, (struct sockaddr *)&name.addr, &name.len) == 0) {
		if (name.addr.ss_family == AF_INET6)
			addr_map_inet6_v4mapped(&tmp.addr, &tmp.len);
	} else {
		JLOG_WARN("getsockname failed, errno=%d", sockerrno);
	}
]=])
                set(after [=[	if (family == AF_UNSPEC) {
		addr_record_t name;
		if (udp_get_bound_addr(sock, &name) == 0)
			family = name.addr.ss_family;
	}
	if (family == AF_INET6)
		addr_map_inet6_v4mapped(&tmp.addr, &tmp.len);
]=])
                string(FIND "${code}" "${after}" position)
                if(position EQUAL -1)
                    string(FIND "${code}" "${before}" position)
                    if(position EQUAL -1)
                        message(FATAL_ERROR "Unexpected pinned UDP address mapping")
                    endif()
                    string(REPLACE "${before}" "${after}" code "${code}")
                endif()
                string(REPLACE "#else\n\treturn sendto(sock, data, size, 0, (const struct sockaddr *)&dst->addr, dst->len);"
                    "#else\n\t(void)family;\n\treturn sendto(sock, data, size, 0, (const struct sockaddr *)&dst->addr, dst->len);"
                    code "${code}")
            endif()
        else()
            string(REGEX REPLACE "udp_sendto\\(([^;\n]+)\\);" "udp_sendto(\\1, AF_UNSPEC);" code "${code}")
            string(REPLACE ", AF_UNSPEC, AF_UNSPEC);" ", AF_UNSPEC);" code "${code}")
            if(source STREQUAL "conn_mux.c")
                string(REPLACE "\tsocket_t sock;\n\tmutex_t send_mutex;"
                    "\tsocket_t sock;\n\tint family;\n\tmutex_t send_mutex;" code "${code}")
                set(before "\tregistry_impl->port = udp_get_port(registry_impl->sock);\n")
                set(after "${before}\taddr_record_t bound;\n\tregistry_impl->family = udp_get_bound_addr(registry_impl->sock, &bound) == 0\n\t    ? bound.addr.ss_family : AF_UNSPEC;\n")
                string(FIND "${code}" "${after}" position)
                if(position EQUAL -1)
                    string(REPLACE "${before}" "${after}" code "${code}")
                endif()
                string(REPLACE "udp_sendto(registry_impl->sock, data, size, dst, AF_UNSPEC)"
                    "udp_sendto(registry_impl->sock, data, size, dst, registry_impl->family)" code "${code}")
                string(REPLACE ", registry_impl->family, AF_UNSPEC);" ", registry_impl->family);" code "${code}")
            endif()
        endif()
        if(NOT code STREQUAL original)
            file(WRITE "${path}" "${code}")
        endif()
    endforeach()
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
