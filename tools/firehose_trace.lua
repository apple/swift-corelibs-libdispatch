#!/usr/local/bin/luatrace -s

trace_codename = function(codename, callback)
	local debugid = trace.debugid(codename)
	if debugid ~= 0 then
		trace.single(debugid,callback)
	else
		printf("WARNING: Cannot locate debugid for '%s'\n", codename)
	end
end

initial_timestamp = 0
get_prefix = function(buf)
	if initial_timestamp == 0 then
		initial_timestamp = buf.timestamp
	end
	local secs = trace.convert_timestamp_to_nanoseconds(buf.timestamp - initial_timestamp) / 1000000000

	local prefix
	if trace.debugid_is_start(buf.debugid) then
		prefix = "→"
	elseif trace.debugid_is_end(buf.debugid) then
		prefix = "←"
	else
		prefix = "↔"
	end

	local proc
	proc = buf.command

	return string.format("%s %6.9f %-17s [%05d.%06x] %-35s",
		prefix, secs, proc, buf.pid, buf.threadid, buf.debugname)
end

decode_stream_state = function(state)
	local reliable_waiters = "-"
	if (state & 0x1) ~= 0 then
		reliable_waiters = "R"
	end
	local unreliable_waiters = "-"
	if (state & 0x2) ~= 0 then
		unreliable_waiters = "U"
	end
	local allocator = state & 0x00000000fffffffc
	local ref = (state & 0x000000ff00000000) >> 32
	local loss = (state & 0x00003f0000000000) >> 40
	local timestamped = "-"
	if (state & 0x0000400000000000) ~= 0 then
		timestamped = "T"
	end
	local waiting_for_logd = "-"
	if (state & 0x0000800000000000) ~= 0 then
		waiting_for_logd = "W"
	end
	local gen = (state & 0xffff000000000000) >> 48
	return string.format("[stream: alloc=0x%08x ref=%u loss=%u gen=%u %s%s%s%s]",
			allocator, ref, loss, gen, reliable_waiters, unreliable_waiters,
			timestamped, waiting_for_logd)
end

trace_codename("DISPATCH_FIREHOSE_TRACE_reserver_gave_up", function(buf)
	printf("%s %s -> %s\n", get_prefix(buf), decode_stream_state(buf[3]),
			decode_stream_state(buf[4]))
end)

trace_codename("DISPATCH_FIREHOSE_TRACE_reserver_wait", function(buf)
	printf("%s %s -> %s\n", get_prefix(buf), decode_stream_state(buf[3]),
			decode_stream_state(buf[4]))
end)

trace_codename("DISPATCH_FIREHOSE_TRACE_allocator", function(buf)
	printf("%s %s -> %s\n", get_prefix(buf), decode_stream_state(buf[3]),
			decode_stream_state(buf[4]))
end)

trace_codename("DISPATCH_FIREHOSE_TRACE_wait_for_logd", function(buf)
	printf("%s %s\n", get_prefix(buf), decode_stream_state(buf[2]))
end)

trace_codename("DISPATCH_FIREHOSE_TRACE_chunk_install", function(buf)
	printf("%s %s -> %s, waited=%u\n", get_prefix(buf), decode_stream_state(buf[3]),
			decode_stream_state(buf[4]), buf[2] >> 63)
end)
