RS485 commands (Channel)

c:clear
Reply: [C] ok:clear rt=1

c:n off
Reply: none

c:n<voice> on <key> [velocity] [@session]
Reply: none

c:n<voice> on <sample> <key> <velocity> @<session>
Reply: none

c:n<voice> off
Reply: none

c:vq
Reply: <active_mask> <sequence> [<voice> <session> <free_samples> <samples_needed_5ms>] x8

c:f <cutoff_hz> [q]
Reply: [C] ok: f <cutoff_hz> Hz [(bypass)] k <k> q <q>

c:f<voice> <cutoff_hz> [q]
Reply: [C] ok: f<voice> base <base_hz> effective <effective_hz> bypass <0|1> k <k> q <q>

c:fk <k>
Reply: [C] ok: fk <k>

c:fk<voice> <k>
Reply: [C] ok: fk<voice> <k>

c:g <channel> <attenuation_db>
Reply: [C] ok

Invalid setting replies: [C] err:syntax or [C] err:range

USB binary messages (Channel)

<BODY 01> <voice> <session> <payload_length> <samples>
Reply: none

<UPLOAD 02> <attack 01> <sample> <total_bytes> <data>
<UPLOAD 02> <wavetable 02> <wave> <total_bytes> <data>
<UPLOAD 02> <program 03> <voice> <total_bytes> <data>
Reply: binary reply with status 0 on success, or status 1 and error text

Success reply: <target> <for=UPLOAD 02> <status 00>
Error reply: <target> <for=UPLOAD 02> <status 01> <error_text> <LF 0A>
