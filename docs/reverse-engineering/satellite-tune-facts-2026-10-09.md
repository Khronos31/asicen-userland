# Original W3U3 satellite tune facts

Static verification against the saved x86_64 `TunerControl.o`; no hardware
reception is established by this note. Addresses below are member .text
offsets. These operations do not call TC_SetLNB.

- `TC_SetFrequency` 2340–2355 accepts satellite RF only on local0 and selects
  source1. Its shared prefix at235a–23b6 writes source demod25=00 then23=4d.
  Source1 selects slave32 (source0 selects30).
- Satellite branch26f0–2738 writes the previously selected BS TSID, or ffff
  above the BS range, then calls `Adpater_SetFreqISDBS`. Successful completion
  writes terrestrial slave30/0f=34 at2750–277a even for satellite, then runs
  source1 ReAcqDemod and writes source demod23=4c at265d–268b.
- `ReAcqDemod` source1 (1018–1038) writes32/03=01.
- `Adpater_SetFreqISDBS`20c0–21fa requires an exact match in24 records of
  `Sat_freq_mapping_list` (.data+0, stride12). Record bytes0..3 are RF kHz,
  little-endian. It sends four satellite tuner messages:
  `[c0, row4, row5, row6, row7]`, `[c0, row8]`, delay10ms,
  `[c0, row9, row10]`, `[c0, row11]`. Each failure stops subsequent messages.
- `TunerRegWrite` source1 (1338–1393) prepends fe to that whole message and
  calls TLIB_I2C_Write(slave32, reg0, mode2). Thus the first byte c0 is already
  the tuner address; unlike terrestrial source0, no c6 byte is prepended.
  Existing mode2 USB staging/send encoding can be reused with slave32.
- `TC_IsLocked`891–8c3 reads32/c3 through DemodRegRead(mode1) and reports lock
  when bit10 is clear. This must only be interpreted after a successful tune
  and ACK; a zeroed failed-read buffer is not a lock.
- `Adapter_TSIDRead`e48–ea8 reads16 bytes from32/ce in mode1 and decodes up to
  eight big-endian16-bit TSIDs. The ASICEN hardware capability is eight slots;
  the px4 client parser accepting slots0..11 does not establish twelve here.
- `Adapter_TSIDWrite`1138–1166 writes the TSID big-endian to32/8f, two bytes,
  through normal DemodRegWrite(mode0).
- `TC_CurrentTSIDRead`ec0–f75 reads32/8f directly in mode0, two bytes; if ffff
  it obtains the first advertised TSID using Adapter_TSIDRead.

RF/IF conversion remains backend-internal: the existing channel-plan RF
formulas document the exact24 frequencies, while px4 clients use satellite IF.
Tune/TSID selection should be tested independently before enabling local0
capture. Existing terrestrial capture code hard-codes local1/endpoint82 in
several places; changing only the supported receiver list would be incorrect.

No conclusion about antenna power, external satellite signal, RF tuner
readback or electrical TS output follows from this static sequence.
# CLI frequency boundary

The imported px4 channel parser exposes satellite **IF kHz**, while the
official W3U3 table uses **RF kHz**. BS01 is1049480 versus11727480;
CS02 is1613000 versus12291000. Both differ by10678000kHz. Future daemon
integration must validate and convert at the hardware boundary, preserving
the existing client/IPC units. A standalone RF diagnostic does not establish
compatibility of the normal BS/CS client path. The imported channel tests
retain the IF expectations.
