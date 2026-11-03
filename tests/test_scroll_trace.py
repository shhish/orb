"""Cadence receipts distinguish observed spans, unknown time, and trace loss."""

import base64
import json
import struct
import zlib

import pytest
from scroll_trace import analyze_trace, main, parse_trace


def receipt(events, *, dropped=0, active=1):
    return (
        "unrelated boot output\n"
        f"@@ORBDEV SCROLLTRACE active={active} count={len(events)} dropped={dropped}\n"
        + "\n".join(
            f"@@ORBDEV SCROLLTRACE event={kind} at_us={at} a={a} b={b}" for kind, at, a, b in events
        )
    )


def test_spans_overlap_without_fictitious_additive_latency():
    result = analyze_trace(
        receipt(
            [
                ("COMPLETE", 10000, 0, 0),
                ("WAIT_BEGIN", 11000, 3000, 1),
                ("WAIT_END", 14000, 3000, 1),
                ("LOOP_BEGIN", 14000, 0, 0),
                ("INPUT_INJECT", 14500, 233, 312),
                ("SCROLL", 15000, 0, 21),
                ("INVALIDATE", 15500, 120, 300),
                ("REFR_START", 16000, 0, 0),
                ("RENDER_START", 16500, 0, 0),
                ("REFR_END", 19000, 0, 0),
                ("LOOP_END", 19500, 5500, 1),
                ("COMPLETE", 20000, 0, 0),
            ]
        )
    )
    assert result["trace_complete"]
    interval = result["completion_intervals"]["frames"][0]
    assert interval["interval_us"] == 10000
    assert interval["observed_inclusive_us"] == {
        "handler": 5500,
        "refresh": 3000,
        "timer_wait": 3000,
    }
    assert interval["outside_observed_refresh_or_wait_us"] == 4000
    assert interval["input_events"] == interval["scroll_events"] == 1
    assert interval["latest_input_age_us"] == 5500
    assert interval["latest_scroll_age_us"] == 5000
    assert interval["latest_invalidation_age_us"] == 4500
    assert result["completion_intervals"]["histogram"]["counts"][10] == 1


def test_unknown_fairness_and_overlapping_waits_are_explicit():
    result = analyze_trace(
        receipt(
            [
                ("COMPLETE", 0, 0, 0),
                ("WAIT_BEGIN", 1000, 1000, 2),
                ("REFR_START", 1500, 0, 0),
                ("WAIT_END", 2500, 1500, 2),
                ("REFR_END", 3000, 0, 0),
                ("WAIT_BEGIN", 4000, 1000, 7),
                ("WAIT_END", 5000, 1000, 7),
                ("COMPLETE", 10000, 0, 0),
            ]
        )
    )
    interval = result["completion_intervals"]["frames"][0]
    assert interval["observed_inclusive_us"]["fairness_wait"] == 1500
    assert interval["observed_inclusive_us"]["other_wait"] == 1000
    # The overlapped refresh and fairness wait cover 2ms, not 3ms.
    assert interval["outside_observed_refresh_or_wait_us"] == 7000
    assert interval["latest_input_age_us"] is None


def test_overflow_keeps_only_traced_intervals_and_partial_endpoints():
    result = analyze_trace(
        receipt(
            [
                ("COMPLETE", 1000, 0, 0),
                ("LOOP_BEGIN", 2000, 0, 0),
                ("COMPLETE", 1000000, 0, 0),
                ("WAIT_BEGIN", 1000001, 1000, 2),
            ],
            dropped=87,
        )
    )
    assert not result["trace_complete"]
    assert result["metadata"]["dropped"] == 87
    assert result["unpaired_endpoints"] == {"LOOP_BEGIN": 1, "WAIT_BEGIN": 1}
    histogram = result["completion_intervals"]["histogram"]
    assert histogram["counts"][-1] == 1
    assert sum(histogram["counts"]) == 1


def test_reordered_cross_core_samples_keep_original_evidence():
    events = [
        ("COMPLETE", 1000, 0, 0),
        ("SCROLL", 1700, 0, 24),
        ("TOUCH_SAMPLE", 1600, 233, 300),
        ("COMPLETE", 2000, 0, 0),
    ]
    result = analyze_trace(receipt(events))
    assert result["events_out_of_timestamp_order"] == 1
    assert result["events"][1]["event"] == "SCROLL"
    assert result["completion_intervals"]["frames"][0]["latest_input_age_us"] == 400


def test_render_start_is_not_paired_with_a_previous_frames_dma_completion():
    result = analyze_trace(
        receipt(
            [
                ("RENDER_START", 1000, 0, 0),
                ("REFR_START", 1800, 0, 0),
                ("RENDER_START", 2000, 0, 0),
                # The next refresh can start before the prior final DMA ends.
                ("COMPLETE", 2100, 0, 0),
                ("REFR_END", 3000, 0, 0),
                ("COMPLETE", 3500, 0, 0),
            ]
        )
    )
    frame = result["completion_intervals"]["frames"][0]
    assert frame["observed_inclusive_us"] == {"refresh": 900}
    assert result["event_counts"]["RENDER_START"] == 2
    assert not result["unpaired_endpoints"]
    assert any("no frame identity" in note for note in result["limitations"])


def test_wait_span_crossing_completions_is_proportioned_to_each_gap():
    result = analyze_trace(
        receipt(
            [
                ("WAIT_BEGIN", 0, 4000, 1),
                ("COMPLETE", 1000, 0, 0),
                ("COMPLETE", 2000, 0, 0),
                ("COMPLETE", 3000, 0, 0),
                ("WAIT_END", 4000, 4000, 1),
                ("COMPLETE", 5000, 0, 0),
            ]
        )
    )
    frames = result["completion_intervals"]["frames"]
    assert [frame["observed_inclusive_us"]["timer_wait"] for frame in frames] == [
        1000,
        1000,
        1000,
    ]
    assert [frame["outside_observed_refresh_or_wait_us"] for frame in frames] == [0, 0, 1000]


def test_full_run_histogram_survives_raw_overflow_as_separate_evidence():
    counts = [0] * 128
    counts[16], counts[127] = 8, 1
    text = receipt([("COMPLETE", 1000, 0, 0), ("COMPLETE", 17000, 0, 0)], dropped=99)
    text = text.replace(
        "active=1",
        "active=1 captured_active=0 started_us=0 ended_us=5000000 completions=10 "
        "loops=100 handler_us=999 timer_wait_us=1234 fairness_wait_us=700",
    )
    text += "\n@@ORBDEV SCROLLTRACE_HIST bucket_us=1000 counts=" + ",".join(map(str, counts))
    result = analyze_trace(text)
    assert result["metadata"]["captured_active"] == 0
    assert result["metadata"]["handler_us"] == 999
    assert not result["trace_complete"]
    assert result["full_run_completion_intervals"]["available"]
    assert result["full_run_completion_intervals"]["histogram"]["counts"] == counts
    assert result["completion_intervals"]["count"] == 1
    assert sum(result["completion_intervals"]["histogram"]["counts"]) == 1


@pytest.mark.parametrize(
    ("histogram", "message"),
    [
        ("bucket_us=1000 counts=0,1", "128 nonnegative"),
        ("bucket_us=2000 counts=" + ",".join(["0"] * 128), "128 nonnegative"),
        ("bucket_us=1000 counts=" + ",".join(["-1"] * 128), "128 nonnegative"),
        ("bucket_us=1000 counts=wat", "invalid histogram"),
        ("bucket_us=1000", "invalid histogram"),
    ],
)
def test_malformed_full_histograms_fail(histogram, message):
    with pytest.raises(ValueError, match=message):
        analyze_trace(receipt([]) + "\n@@ORBDEV SCROLLTRACE_HIST " + histogram)


def test_histogram_count_must_match_full_run_completions():
    text = receipt([]).replace("active=1", "active=1 completions=10")
    text += "\n@@ORBDEV SCROLLTRACE_HIST bucket_us=1000 counts=" + ",".join(["0"] * 128)
    with pytest.raises(ValueError, match="completions - 1"):
        analyze_trace(text)


def test_duplicate_full_histograms_fail():
    histogram = "\n@@ORBDEV SCROLLTRACE_HIST bucket_us=1000 counts=" + ",".join(["0"] * 128)
    with pytest.raises(ValueError, match="multiple SCROLLTRACE_HIST"):
        analyze_trace(receipt([]) + 2 * histogram)


def test_unavailable_capture_does_not_claim_empty_histogram_was_measured():
    histogram = "\n@@ORBDEV SCROLLTRACE_HIST bucket_us=1000 counts=" + ",".join(["0"] * 128)
    result = analyze_trace(receipt([], active=0) + histogram)
    assert not result["full_run_completion_intervals"]["available"]
    assert result["full_run_completion_intervals"]["histogram"] is None


def test_still_running_capture_is_not_complete():
    result = analyze_trace(receipt([]).replace("active=1", "active=1 captured_active=1"))
    assert not result["trace_complete"]


def test_fixed_event_buffer_upper_bound_keeps_frame_and_sample_accounting():
    # Exercise the worst completion/event ratio in the bounded 16K buffer.
    events = []
    for frame in range(8192):
        events += [("SCROLL", frame * 1000, 0, frame), ("COMPLETE", frame * 1000 + 500, 0, 0)]
    result = analyze_trace(receipt(events))
    intervals = result["completion_intervals"]
    assert intervals["count"] == 8191
    assert intervals["histogram"]["counts"][1] == 8191
    assert all(frame["scroll_events"] == 1 for frame in intervals["frames"])


@pytest.mark.parametrize("active", [0, 1])
def test_no_completions_is_unavailable_not_zero_latency(active):
    result = analyze_trace(receipt([], active=active))
    intervals = result["completion_intervals"]
    assert intervals["count"] == 0
    assert intervals["minimum_us"] is None
    assert intervals["maximum_us"] is None
    assert not intervals["frames"]
    assert not any(intervals["histogram"]["counts"])


@pytest.mark.parametrize(
    ("text", "message"),
    [
        ("no trace", "no SCROLLTRACE"),
        ("@@ORBDEV SCROLLTRACE active=1 count=1 dropped=0", "incomplete receipt"),
        ("@@ORBDEV SCROLLTRACE active=1 count=-1 dropped=0", "nonnegative"),
        ("@@ORBDEV SCROLLTRACE active=1 count=0 dropped=0 count=1", "repeated field"),
        ("@@ORBDEV SCROLLTRACE active=2 count=0 dropped=0", "active must"),
        ("@@ORBDEV SCROLLTRACE active=1 count=wat dropped=0", "invalid integer"),
        ("@@ORBDEV SCROLLTRACE active=1 count=0 dropped=0 damage", "key=value"),
        (receipt([], active=0, dropped=1), "inactive trace"),
        (receipt([]) + "\n" + receipt([]), "multiple SCROLLTRACE"),
        (
            "@@ORBDEV SCROLLTRACE active=1 count=1 dropped=0\n"
            "@@ORBDEV SCROLLTRACE event=SCROLL at_us=-1 a=0 b=1",
            "nonnegative",
        ),
    ],
)
def test_damaged_or_ambiguous_receipts_are_rejected(text, message):
    with pytest.raises(ValueError, match=message):
        parse_trace(text)


def test_cli_writes_standalone_json_and_reports_loss(tmp_path, capsys):
    source = tmp_path / "scroll.txt"
    source.write_text(receipt([("COMPLETE", 1000, 0, 0)], dropped=1))
    output = tmp_path / "analysis" / "scroll.json"
    main([str(source), "--output", str(output)])
    assert json.loads(output.read_text())["metadata"]["dropped"] == 1
    assert "trace_complete=0; dropped=1" in capsys.readouterr().out


def test_cli_reports_missing_debug_trace_as_an_error(tmp_path, capsys):
    source = tmp_path / "scroll.txt"
    source.write_text("@@ORBDEV SCROLL surface=agents fps=50\n")
    with pytest.raises(SystemExit) as error:
        main([str(source), "--output", str(tmp_path / "out.json")])
    assert error.value.code == 2
    assert "enable CONFIG_ORB_DISPLAY_SCROLL_TRACE" in capsys.readouterr().err


def packed_receipt(events, *, dropped=0, active=1):
    """Fixture bytes match the explicit firmware le13 serialization."""
    from scroll_trace import EVENT_NAMES

    started = 123456789
    raw = b"".join(
        struct.pack("<IiiB", at, a, b, EVENT_NAMES.index(kind)) for kind, at, a, b in events
    )
    blocks = [raw[offset : offset + 192] for offset in range(0, len(raw), 192)]
    crc = zlib.crc32(raw)
    completions = [at for kind, at, _a, _b in events if kind == "COMPLETE"]
    counts = [0] * 128
    for left, right in zip(completions, completions[1:]):
        counts[min((right - left) // 1000, 127)] += 1
    ended = started + max(2000000, max((at for _kind, at, _a, _b in events), default=0))
    histogram = (
        "@@ORBDEV SCROLLTRACE_HIST bucket_us=1000 counts=" + ",".join(map(str, counts)) + "\n"
    )
    header = (
        f"@@ORBDEV SCROLLTRACE version=2 encoding=le13 record_bytes=13 "
        f"active={active} captured_active=0 count={len(events)} dropped={dropped} "
        f"bytes={len(raw)} chunks={len(blocks)} crc32={crc:08x} "
        f"started_us={started} ended_us={ended} completions={len(completions)} "
        "loops=0 handler_us=0 timer_wait_us=0 fairness_wait_us=0\n"
    )
    chunks = "".join(
        f"@@ORBDEV SCROLLTRACE_CHUNK seq={seq} bytes={len(block)} "
        f"crc32={zlib.crc32(block):08x} data={base64.b64encode(block).decode()}\n"
        for seq, block in enumerate(blocks)
    )
    trailer = (
        f"@@ORBDEV SCROLLTRACE_END chunks={len(blocks)} bytes={len(raw)} "
        f"crc32={crc:08x} emit_us=5000000\n"
    )
    return header + histogram + chunks + trailer


def test_packed_capture_reassembles_records_across_chunk_boundaries():
    events = [("SCROLL", index * 16000, -index, index) for index in range(70)]
    metadata, captured = parse_trace(packed_receipt(events, dropped=7))
    assert metadata["emit_us"] == 5000000
    assert metadata["dropped"] == 7
    assert captured == [
        {"event": kind, "at_us": 123456789 + at, "a": a, "b": b} for kind, at, a, b in events
    ]


def test_packed_chunks_accept_indexed_out_of_order_delivery():
    text = packed_receipt([("SCROLL", i, i, -i) for i in range(70)])
    header, *chunks, trailer = text.splitlines()
    assert parse_trace("\n".join([header, *reversed(chunks), trailer])) == parse_trace(text)


@pytest.mark.parametrize("active", [0, 1])
def test_packed_empty_capture_is_complete_with_explicit_end(active):
    result = analyze_trace(packed_receipt([], active=active))
    assert result["trace_complete"] == bool(active)
    assert not result["events"]


def test_packed_maximum_capture_stays_bounded_and_decodes_signed_fields():
    events = [("SCROLL", index * 1000, -2147483648, 2147483647) for index in range(2048)]
    text = packed_receipt(events)
    assert len(text.encode()) < 50000
    assert len(parse_trace(text)[1]) == 2048


@pytest.mark.parametrize(
    "damage, message",
    [
        (lambda text: text.replace("encoding=le13", "encoding=unknown"), "encoding"),
        (lambda text: text.replace("record_bytes=13", "record_bytes=24"), "record_bytes"),
        (lambda text: text.replace("version=2", "version=3"), "version"),
        (lambda text: text.replace("count=70", "count=2049"), "bounded"),
        (lambda text: text.replace("bytes=910 chunks=5", "bytes=911 chunks=5", 1), "bounded"),
        (lambda text: text.replace("seq=1", "seq=0"), "duplicate"),
        (lambda text: text.replace("seq=1", "seq=139"), "bound"),
        (
            lambda text: "\n".join(line for line in text.splitlines() if "seq=1 " not in line),
            "missing",
        ),
        (lambda text: text.split("@@ORBDEV SCROLLTRACE_END")[0], "missing SCROLLTRACE_END"),
        (
            lambda text: text + text[text.index("@@ORBDEV SCROLLTRACE_END") :],
            "multiple SCROLLTRACE_END",
        ),
        (
            lambda text: text.replace("SCROLLTRACE_END chunks=5", "SCROLLTRACE_END chunks=4"),
            "differs",
        ),
        (lambda text: text.replace("data=", "data=!", 1), "size bound"),
        (lambda text: text.replace("bytes=192 crc32=", "bytes=191 crc32=", 1), "length/CRC"),
        (lambda text: text + "@@ORBDEV SCROLLTRACE event=SCROLL at_us=1 a=2 b=3\n", "mixes"),
    ],
)
def test_damaged_packed_capture_fails_closed(damage, message):
    text = packed_receipt([("SCROLL", i, i, -i) for i in range(70)])
    with pytest.raises(ValueError, match=message):
        parse_trace(damage(text))


def test_packed_bad_chunk_crc_and_payload_fail():
    lines = packed_receipt([("SCROLL", 1, 2, 3)]).splitlines()
    fields = lines[2].split()
    fields[-2] = "crc32=00000000"
    lines[2] = " ".join(fields)
    with pytest.raises(ValueError, match="length/CRC"):
        parse_trace("\n".join(lines))
    fields[-1] = "data=invalid!"
    lines[2] = " ".join(fields)
    with pytest.raises(ValueError, match="base64"):
        parse_trace("\n".join(lines))


def test_packed_full_crc_rejects_consistently_rewritten_header_and_end():
    text = packed_receipt([("SCROLL", 1, 2, 3)])
    raw = struct.pack("<IiiB", 1, 2, 3, 7)
    crc = f"{zlib.crc32(raw):08x}"
    lines = text.splitlines()
    lines[0] = lines[0].replace(crc, "00000000")
    lines[-1] = lines[-1].replace(crc, "00000000")
    with pytest.raises(ValueError, match="full CRC"):
        parse_trace("\n".join(lines))


def test_packed_unknown_kind_is_not_silently_reinterpreted():
    raw = struct.pack("<IiiB", 1, 2, 3, 255)
    old = struct.pack("<IiiB", 1, 2, 3, 7)
    text = (
        packed_receipt([("SCROLL", 1, 2, 3)])
        .replace(base64.b64encode(old).decode(), base64.b64encode(raw).decode())
        .replace(f"{zlib.crc32(old):08x}", f"{zlib.crc32(raw):08x}")
    )
    with pytest.raises(ValueError, match="unknown event"):
        parse_trace(text)


def test_packed_header_requires_full_histogram_and_stopped_capture():
    text = packed_receipt([("SCROLL", 100, 1, 2)])
    with pytest.raises(ValueError, match="missing SCROLLTRACE_HIST"):
        parse_trace(
            "\n".join(line for line in text.splitlines() if "SCROLLTRACE_HIST " not in line)
        )
    with pytest.raises(ValueError, match="still active"):
        parse_trace(text.replace("captured_active=0", "captured_active=1"))
    with pytest.raises(ValueError, match="invalid end"):
        parse_trace(text.replace("ended_us=125456789", "ended_us=1"))
    with pytest.raises(ValueError, match="follows capture end"):
        parse_trace(text.replace("ended_us=125456789", "ended_us=123456790"))
