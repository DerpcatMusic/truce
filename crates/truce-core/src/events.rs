//! Event types crossing the host → plugin boundary.
//!
//! `EventBody` carries MIDI 1.0 and MIDI 2.0 channel-voice messages
//! in their **wire-native integer** shapes (7-bit `u8`, 14-bit
//! `u16`, 16-bit `u16`, 32-bit `u32`) so the framework's
//! representation round-trips exactly with the host's wire format.
//! Plugin code that wants float values reaches for the helpers in
//! [`truce_utils::midi`] (`norm_7bit`, `denorm_7bit`,
//! `norm_pitch_bend`, `denorm_pitch_bend`).
//!
//! Every MIDI variant carries a `group: u8` field (0..=15) that
//! UMP (Universal MIDI Packet) hosts use to address one of 16
//! groups × 16 channels = 256 logical channels. Format wrappers
//! that don't expose the group field (legacy MIDI 1.0 byte streams)
//! emit `0`.

/// A timestamped event within a process block.
///
/// `Copy` because every [`EventBody`] variant is POD - lets the
/// audio path move events without per-event clones.
#[derive(Clone, Copy, Debug)]
pub struct Event {
    /// Sample offset within the block (`0..num_samples`).
    pub sample_offset: u32,
    /// MIDI port this event arrived on / goes out on (0-based). Single-
    /// port plugins - the vast majority - always see `0` and can ignore
    /// it. A plugin that declares more than one MIDI port (see
    /// `PluginInfo::midi_input_ports` / `midi_output_ports`) filters
    /// inbound events by `port` and stamps outbound ones with the port
    /// they should leave on. Formats without a multi-port MIDI transport
    /// clamp everything to `0`.
    pub port: u8,
    pub body: EventBody,
}

impl Event {
    /// Event on the default MIDI port (`0`). The common constructor -
    /// single-port plugins and every non-MIDI event use this.
    #[must_use]
    pub fn new(sample_offset: u32, body: EventBody) -> Self {
        Self {
            sample_offset,
            port: 0,
            body,
        }
    }

    /// Event addressed to / from a specific MIDI port. Only meaningful
    /// for plugins that declared more than one MIDI port; wrappers on
    /// single-port formats route it to port `0` regardless.
    #[must_use]
    pub fn on_port(sample_offset: u32, port: u8, body: EventBody) -> Self {
        Self {
            sample_offset,
            port,
            body,
        }
    }
}

/// A validated raw MIDI 1.0 short message.
///
/// The length is part of the value because system-common messages may use
/// fewer than three bytes. SysEx remains in [`EventBody::SysEx`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RawMidi1 {
    bytes: [u8; 3],
    len: u8,
}

impl RawMidi1 {
    /// Construct a one-, two-, or three-byte MIDI 1.0 message.
    #[must_use]
    pub fn new(bytes: [u8; 3], len: u8) -> Option<Self> {
        (1..=3).contains(&len).then_some(Self { bytes, len })
    }

    /// The meaningful bytes in this message.
    #[must_use]
    pub fn bytes(&self) -> &[u8] {
        &self.bytes[..usize::from(self.len)]
    }

    /// Original three-byte storage, including trailing bytes outside the
    /// semantic message length. Useful for fixed-width host transports.
    #[must_use]
    pub fn storage(&self) -> &[u8; 3] {
        &self.bytes
    }

    #[must_use]
    pub fn len(&self) -> usize {
        usize::from(self.len)
    }

    #[must_use]
    pub fn is_empty(&self) -> bool {
        false
    }
}

/// A validated Universal MIDI Packet containing one to four 32-bit words.
///
/// No message-type filtering is performed: utility, system, data, flex-data,
/// stream, and future packets remain byte-for-byte observable.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RawUmp {
    words: [u32; 4],
    word_count: u8,
}

impl RawUmp {
    /// Construct a UMP with its original packet length.
    #[must_use]
    pub fn new(words: [u32; 4], word_count: u8) -> Option<Self> {
        (1..=4)
            .contains(&word_count)
            .then_some(Self { words, word_count })
    }

    /// The meaningful words in this packet.
    #[must_use]
    pub fn words(&self) -> &[u32] {
        &self.words[..usize::from(self.word_count)]
    }

    /// Original four-word storage, including words outside the packet length.
    #[must_use]
    pub fn storage(&self) -> &[u32; 4] {
        &self.words
    }

    #[must_use]
    pub fn word_count(&self) -> usize {
        usize::from(self.word_count)
    }
}

/// Format-neutral note addressing. `None` means the host wildcard for that
/// component; `Some` preserves a concrete address exactly.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ExactNoteAddress {
    pub port: Option<u16>,
    pub channel: Option<u8>,
    pub key: Option<u8>,
    pub note_id: Option<i32>,
}

/// A note lifecycle operation which may not have a MIDI byte equivalent.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[non_exhaustive]
pub enum ExactNoteKind {
    On,
    Off,
    Choke,
    End,
}

/// Lossless, format-neutral event payloads kept alongside the convenient
/// [`EventBody`] representation.
#[derive(Clone, Copy, Debug, PartialEq)]
#[non_exhaustive]
pub enum ExactEventBody {
    Midi1 {
        port: u16,
        message: RawMidi1,
    },
    Ump {
        port: u16,
        packet: RawUmp,
    },
    Note {
        kind: ExactNoteKind,
        address: ExactNoteAddress,
        velocity: f64,
    },
    NoteExpression {
        expression_id: i32,
        address: ExactNoteAddress,
        value: f64,
    },
}

/// A timestamped lossless event. Exact-only events have no [`EventBody`]
/// fallback; linked events expose one through [`ExactEventRef::fallback`].
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ExactEvent {
    sample_offset: u32,
    pub body: ExactEventBody,
}

impl ExactEvent {
    #[must_use]
    pub fn new(sample_offset: u32, body: ExactEventBody) -> Self {
        Self {
            sample_offset,
            body,
        }
    }

    #[must_use]
    pub fn sample_offset(&self) -> u32 {
        self.sample_offset
    }
}

/// Borrowed view of an exact event and its optional typed fallback.
#[derive(Clone, Copy, Debug)]
pub struct ExactEventRef<'a> {
    exact: &'a ExactEvent,
    fallback: Option<&'a Event>,
}

/// Allocation-free merged view of exact events and typed events which do not
/// have an exact payload. Linked typed fallbacks appear only as [`Self::Exact`]
/// so an adapter can replay the lossless payload without emitting a duplicate.
#[derive(Clone, Copy, Debug)]
pub enum LosslessEventRef<'a> {
    Typed(&'a Event),
    Exact(ExactEventRef<'a>),
}

impl<'a> ExactEventRef<'a> {
    #[must_use]
    pub fn body(self) -> &'a ExactEventBody {
        &self.exact.body
    }

    #[must_use]
    pub fn fallback(self) -> Option<&'a Event> {
        self.fallback
    }

    /// Effective timestamp. A linked exact payload follows the fallback's
    /// timestamp, including mutations made through [`EventList::events_mut`].
    #[must_use]
    pub fn sample_offset(self) -> u32 {
        self.fallback
            .map_or(self.exact.sample_offset, |event| event.sample_offset)
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum EventBody {
    // -- MIDI 1.0 channel voice (wire-native 7-bit / 14-bit) --
    /// Note on. MIDI 1.0 quirk: a `NoteOn` with `velocity == 0` is
    /// a `NoteOff`. Format wrappers normalize that at parse time so
    /// plugin code can match `NoteOn` without checking velocity.
    NoteOn {
        group: u8,
        channel: u8,
        note: u8,
        velocity: u8,
    },
    NoteOff {
        group: u8,
        channel: u8,
        note: u8,
        velocity: u8,
    },
    /// Polyphonic key pressure (per-note aftertouch).
    Aftertouch {
        group: u8,
        channel: u8,
        note: u8,
        pressure: u8,
    },
    ChannelPressure {
        group: u8,
        channel: u8,
        pressure: u8,
    },
    ControlChange {
        group: u8,
        channel: u8,
        cc: u8,
        value: u8,
    },
    /// 14-bit pitch bend, raw code `0..=16383`. `8192` is center.
    /// See `truce_utils::midi::norm_pitch_bend` for the
    /// asymmetric-range conversion helper.
    PitchBend {
        group: u8,
        channel: u8,
        value: u16,
    },
    ProgramChange {
        group: u8,
        channel: u8,
        program: u8,
    },

    // -- MIDI 2.0 channel voice (wire-native 16/32-bit) --
    /// MIDI 2.0 `NoteOn`. `velocity` is `0..=65535`; unlike MIDI 1.0,
    /// a zero velocity is a genuine zero (`NoteOff` is its own
    /// dedicated message). `attribute_type` indicates how
    /// `attribute` should be interpreted: 0 = no attribute, 1 =
    /// manufacturer-specific, 2 = profile-specific, 3 = Pitch 7.9.
    NoteOn2 {
        group: u8,
        channel: u8,
        note: u8,
        velocity: u16,
        attribute_type: u8,
        attribute: u16,
    },
    NoteOff2 {
        group: u8,
        channel: u8,
        note: u8,
        velocity: u16,
        attribute_type: u8,
        attribute: u16,
    },
    /// MIDI 2.0 polyphonic key pressure (`pressure: u32`).
    PolyPressure2 {
        group: u8,
        channel: u8,
        note: u8,
        pressure: u32,
    },
    /// MIDI 2.0 per-note controller. `registered = true` for
    /// Registered Per-Note (RPN-like indexed list); `false` for
    /// Assignable Per-Note (free-form per-controller mapping).
    PerNoteCC {
        group: u8,
        channel: u8,
        note: u8,
        cc: u8,
        value: u32,
        registered: bool,
    },
    /// MIDI 2.0 per-note pitch bend (`value: u32`). `0x8000_0000`
    /// is center; full-scale is ±48 semitones
    /// ([`crate::midi::PER_NOTE_TUNING_SEMITONES`]) wherever a
    /// wrapper maps it onto a semitone-denominated host domain.
    PerNotePitchBend {
        group: u8,
        channel: u8,
        note: u8,
        value: u32,
    },
    /// MIDI 2.0 per-note management flags. Bit 0 = detach
    /// per-note controllers from active note; bit 1 = reset
    /// (set) per-note controllers to default values.
    PerNoteManagement {
        group: u8,
        channel: u8,
        note: u8,
        flags: u8,
    },
    /// MIDI 2.0 channel-wide control change (32-bit).
    ControlChange2 {
        group: u8,
        channel: u8,
        cc: u8,
        value: u32,
    },
    /// MIDI 2.0 channel pressure (32-bit aftertouch on the whole
    /// channel).
    ChannelPressure2 {
        group: u8,
        channel: u8,
        pressure: u32,
    },
    /// MIDI 2.0 channel pitch bend (32-bit). `0x8000_0000` is
    /// center.
    PitchBend2 {
        group: u8,
        channel: u8,
        value: u32,
    },
    /// MIDI 2.0 program change. Optional bank pair (MSB, LSB);
    /// MIDI 2.0's "B" flag is encoded as `Some` / `None`. When
    /// `None`, the host hasn't selected a bank and the program
    /// applies in the current bank.
    ProgramChange2 {
        group: u8,
        channel: u8,
        program: u8,
        bank: Option<(u8, u8)>,
    },
    /// MIDI 2.0 Registered Controller (the spec's RPN replacement,
    /// 32-bit). `bank` and `index` are the two 7-bit identifiers
    /// the spec reserves for Registered Parameter Numbers.
    RegisteredController {
        group: u8,
        channel: u8,
        bank: u8,
        index: u8,
        value: u32,
    },
    /// MIDI 2.0 Assignable Controller (the spec's NRPN
    /// replacement, 32-bit). `bank` and `index` are
    /// manufacturer-defined.
    AssignableController {
        group: u8,
        channel: u8,
        bank: u8,
        index: u8,
        value: u32,
    },

    // -- truce-internal automation --
    ParamChange {
        id: u32,
        value: f64,
    },
    /// Parameter modulation offset (CLAP-specific, zero on other
    /// formats). Effective value is `base + value`. The base value
    /// is unchanged.
    ParamMod {
        id: u32,
        note_id: i32,
        value: f64,
    },

    // -- Transport --
    Transport(TransportInfo),

    // -- System layer --
    /// System Exclusive (`SysEx`) message - MIDI 1.0 and MIDI 2.0
    /// alike. The payload bytes live in [`EventList::sysex_bytes`];
    /// resolve a body to its slice with
    /// `event_list.sysex_bytes(&body)` rather than indexing the
    /// pool directly. The bytes are the inner `SysEx` data
    /// **without** the leading `0xF0` start byte or trailing `0xF7`
    /// end byte - format wrappers strip those at the boundary so
    /// plugin code doesn't have to.
    ///
    /// Inlining the bytes in the variant would balloon every event's
    /// footprint to the worst-case (~64 KiB) - channel-voice events
    /// are <8 bytes today and we want to keep the per-event memory
    /// pressure on the audio thread proportional to that. The
    /// indices-into-a-pool layout pays the price (two-step access)
    /// for the `SysEx`-handling path only.
    SysEx {
        pool_offset: u32,
        len: u32,
    },
}

/// Host-populated transport snapshot. Constructed by every format
/// wrapper from the host's own transport struct via struct-literal
/// expressions, so this stays "exhaustive" (no `#[non_exhaustive]`,
/// which would block cross-crate construction). Adding a new field
/// is a coordinated workspace-wide change.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct TransportInfo {
    pub playing: bool,
    pub recording: bool,
    pub tempo: f64,
    pub time_sig_num: u8,
    pub time_sig_den: u8,
    pub position_samples: i64,
    pub position_seconds: f64,
    pub position_beats: f64,
    pub bar_start_beats: f64,
    pub loop_active: bool,
    pub loop_start_beats: f64,
    pub loop_end_beats: f64,
}

impl TransportInfo {
    /// Synthetic transport for snapshot tests - playing at 120 BPM,
    /// 4/4, position 4.0 beats. Used as the default by every snapshot
    /// helper (`truce-egui`, `truce-slint`, `truce-iced`,
    /// `truce-test`) so that transport-aware widgets render a
    /// populated readout in marketing screenshots instead of a
    /// `(no host transport)` placeholder.
    #[must_use]
    pub fn for_screenshot() -> Self {
        Self {
            playing: true,
            tempo: 120.0,
            time_sig_num: 4,
            time_sig_den: 4,
            position_beats: 4.0,
            // 4 beats at 120 BPM is 2.0 s = 96000 samples at 48 kHz;
            // keeps the sample + beat positions consistent in readouts.
            position_samples: 96_000,
            ..Self::default()
        }
    }
}

/// Default reserved capacity for per-instance `EventList`s held by
/// format wrappers. Sized to cover a heavy MIDI block (note bursts +
/// per-block automation changes) without growing past steady state.
///
/// Plugins can construct a smaller or larger list explicitly via
/// [`EventList::with_capacity`]; this const exists so the format
/// wrappers don't each pick their own magic number.
pub const EVENT_LIST_PREALLOC: usize = 256;

/// Default reserved capacity for the `SysEx` byte pool on
/// per-instance `EventList`s. 128 KiB ≈ 2× the worst-case single
/// payload (one 64 KiB firmware-update-shaped message) with
/// headroom for an interleaved burst of small messages in the
/// same block.
///
/// Sized at construction in [`EventList::with_capacity`]; never
/// re-allocates on the audio thread. A plugin that pushes beyond
/// this gets a [`PushError::PoolFull`] and the message is dropped;
/// truncating or splitting a `SysEx` makes it invalid.
///
/// Must agree with the `TRUCE_SYSEX_POOL_PREALLOC` C macro in the
/// shared shim header: the AU v3 Swift template (which can't import
/// Rust consts) reads the C macro to size its per-render output
/// scratch buffer, and a per-format unit test asserts the two values
/// match.
pub const SYSEX_POOL_PREALLOC: usize = 128 * 1024;

/// Why a push into the bounded [`EventList`] failed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PushError {
    /// The typed event lane is full.
    EventFull,
    /// The exact event lane is full.
    ExactEventFull,
    /// The `SysEx` byte pool is full. The message wasn't appended.
    /// Callers either drop it, surface it via a meter, or bump the
    /// pool size via [`EventList::with_capacity`] at construction.
    PoolFull,
}

impl core::fmt::Display for PushError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            Self::EventFull => f.write_str("event lane is full"),
            Self::ExactEventFull => f.write_str("exact event lane is full"),
            Self::PoolFull => f.write_str("SysEx byte pool is full"),
        }
    }
}

impl std::error::Error for PushError {}

/// Ordered list of events within a process block.
///
/// `events` and `exact_events` are the per-block event lanes; `sysex_pool`
/// is the variable-byte arena that [`EventBody::SysEx`] entries index into.
/// All are pre-allocated by [`EventList::with_capacity`] and reset
/// (length only - backing memory preserved) by [`Self::clear`], so
/// steady-state operation is allocation-free.
#[derive(Debug)]
pub struct EventList {
    events: Vec<Event>,
    event_exact: Vec<Option<usize>>,
    event_sequence: Vec<u64>,
    exact_events: Vec<StoredExactEvent>,
    sysex_pool: Vec<u8>,
    overflow: Option<PushError>,
    next_sequence: u64,
}

#[derive(Clone, Debug)]
struct StoredExactEvent {
    event: ExactEvent,
    fallback_index: Option<usize>,
    sequence: u64,
}

impl Clone for EventList {
    fn clone(&self) -> Self {
        Self {
            events: clone_vec_preserving_capacity(&self.events),
            event_exact: clone_vec_preserving_capacity(&self.event_exact),
            event_sequence: clone_vec_preserving_capacity(&self.event_sequence),
            exact_events: clone_vec_preserving_capacity(&self.exact_events),
            sysex_pool: clone_vec_preserving_capacity(&self.sysex_pool),
            overflow: self.overflow,
            next_sequence: self.next_sequence,
        }
    }
}

fn clone_vec_preserving_capacity<T: Clone>(source: &Vec<T>) -> Vec<T> {
    let mut cloned = Vec::with_capacity(source.capacity());
    cloned.extend_from_slice(source);
    cloned
}

impl Default for EventList {
    fn default() -> Self {
        Self::with_capacity(EVENT_LIST_PREALLOC)
    }
}

impl EventList {
    /// Construct an `EventList` with backing capacity already reserved.
    ///
    /// Format wrappers build their per-instance event lists at construction
    /// time and reuse them across blocks via `clear()`. [`Self::default`]
    /// reserves [`EVENT_LIST_PREALLOC`]; use this constructor when the host's
    /// maximum event count is known.
    ///
    /// The `SysEx` byte pool is sized to [`SYSEX_POOL_PREALLOC`]
    /// regardless of `capacity` - `capacity` controls both event lanes.
    #[must_use]
    pub fn with_capacity(capacity: usize) -> Self {
        Self {
            events: Vec::with_capacity(capacity),
            event_exact: Vec::with_capacity(capacity),
            event_sequence: Vec::with_capacity(capacity),
            exact_events: Vec::with_capacity(capacity),
            sysex_pool: Vec::with_capacity(SYSEX_POOL_PREALLOC),
            overflow: None,
            next_sequence: 0,
        }
    }

    /// Append an event. Note: `sample_offset` is **not** bounds-checked
    /// against any block size - callers that build event lists per
    /// block must validate `sample_offset < num_samples` themselves
    /// (the audio thread can't recover from an out-of-range offset, so
    /// we treat that as a contract violation rather than panicking).
    pub fn push(&mut self, event: Event) {
        let _ = self.try_push(event);
    }

    /// Fallible form of [`Self::push`]. No lane grows beyond the capacity
    /// reserved at construction.
    ///
    /// # Errors
    /// [`PushError::EventFull`] when the typed lane is full.
    pub fn try_push(&mut self, event: Event) -> Result<(), PushError> {
        self.reserve_event_slot()?;
        let sequence = self.take_sequence();
        self.events.push(event);
        self.event_exact.push(None);
        self.event_sequence.push(sequence);
        Ok(())
    }

    /// Append an exact-only event which has no typed fallback.
    ///
    /// # Errors
    /// [`PushError::ExactEventFull`] when the exact lane is full.
    pub fn try_push_exact(&mut self, event: ExactEvent) -> Result<(), PushError> {
        self.reserve_exact_slot()?;
        let sequence = self.take_sequence();
        self.exact_events.push(StoredExactEvent {
            event,
            fallback_index: None,
            sequence,
        });
        Ok(())
    }

    /// Atomically append a typed convenience event and its exact payload.
    /// The exact timestamp is anchored to the typed event so subsequent
    /// offset mutation cannot split the pair.
    ///
    /// # Errors
    /// [`PushError::EventFull`] or [`PushError::ExactEventFull`] when the
    /// corresponding lane is full. Neither lane changes on failure.
    pub fn try_push_with_exact(
        &mut self,
        event: Event,
        mut exact: ExactEvent,
    ) -> Result<(), PushError> {
        self.reserve_event_slot()?;
        self.reserve_exact_slot()?;

        exact.sample_offset = event.sample_offset;
        let event_index = self.events.len();
        let exact_index = self.exact_events.len();
        let sequence = self.take_sequence();
        self.events.push(event);
        self.event_exact.push(Some(exact_index));
        self.event_sequence.push(sequence);
        self.exact_events.push(StoredExactEvent {
            event: exact,
            fallback_index: Some(event_index),
            sequence,
        });
        Ok(())
    }

    /// Sort the list by `sample_offset` if it isn't already, keeping
    /// the push order of equal-offset events (a recentre bend must stay
    /// ahead of the note-off it precedes). Hosts require output queues
    /// ordered by time; wrappers call this before draining so a plugin
    /// that pushed block-level events after per-event ones can't hand
    /// the host an unsorted queue; wrappers also sort the merged
    /// *input* stream before processing. Audio-thread safe: the
    /// common already-sorted case is one linear scan, and the fix-up
    /// is an in-place insertion sort - no allocation (`sort_by_key`
    /// is a std stable sort that allocates past ~20 elements;
    /// `sort_unstable` would reorder equal offsets, and stability is
    /// load-bearing: "note-on then CC on the same sample" must stay
    /// in push order). Linked exact payloads follow their fallback while
    /// exact-only events are sorted in their own lane. `SysEx` pool offsets
    /// stay valid because the pool's bytes aren't moved.
    pub fn ensure_sorted_by_offset(&mut self) {
        if !self.events.is_sorted_by_key(|event| event.sample_offset) {
            for i in 1..self.events.len() {
                let mut j = i;
                while j > 0 && self.events[j - 1].sample_offset > self.events[j].sample_offset {
                    self.events.swap(j - 1, j);
                    self.event_exact.swap(j - 1, j);
                    self.event_sequence.swap(j - 1, j);
                    j -= 1;
                }
            }
        }

        for (event_index, exact_index) in self.event_exact.iter().copied().enumerate() {
            if let Some(stored) = exact_index.and_then(|index| self.exact_events.get_mut(index)) {
                stored.fallback_index = Some(event_index);
            }
        }

        for i in 1..self.exact_events.len() {
            let mut j = i;
            while j > 0
                && Self::exact_offset(&self.exact_events[j - 1], &self.events)
                    > Self::exact_offset(&self.exact_events[j], &self.events)
            {
                self.exact_events.swap(j - 1, j);
                j -= 1;
            }
        }

        for link in &mut self.event_exact {
            if link.is_some() {
                *link = None;
            }
        }
        for (exact_index, stored) in self.exact_events.iter().enumerate() {
            if let Some(link) = stored
                .fallback_index
                .and_then(|event_index| self.event_exact.get_mut(event_index))
            {
                *link = Some(exact_index);
            }
        }
    }

    /// Append a `SysEx` event whose payload is copied into the pool.
    /// `data` is the inner `SysEx` bytes **without** the leading
    /// `0xF0` / trailing `0xF7` - wrappers strip those at the
    /// boundary.
    ///
    /// Returns [`PushError::PoolFull`] when the pool can't hold
    /// `data.len()` more bytes; the event is *not* appended and the
    /// pool is left unchanged. `SysEx` messages are atomic by spec,
    /// so the caller's choices are drop-and-flag (via a meter) or
    /// fail the host call. Splitting / truncating produces a corrupt
    /// message and is never the right answer.
    ///
    /// # Errors
    /// [`PushError::EventFull`] when the event lane is full, or
    /// [`PushError::PoolFull`] when the byte pool is at capacity.
    pub fn push_sysex(&mut self, sample_offset: u32, data: &[u8]) -> Result<(), PushError> {
        self.push_sysex_on_port(sample_offset, 0, data)
    }

    /// Like [`Self::push_sysex`] but stamps the event with a MIDI
    /// [`Event::port`]. Single-port callers use [`Self::push_sysex`]
    /// (port `0`); a multi-port wrapper preserves the port a `SysEx`
    /// arrived on.
    ///
    /// # Errors
    /// [`PushError::EventFull`] when the event lane is full, or
    /// [`PushError::PoolFull`] when the byte pool is at capacity.
    pub fn push_sysex_on_port(
        &mut self,
        sample_offset: u32,
        port: u8,
        data: &[u8],
    ) -> Result<(), PushError> {
        self.push_sysex_on_port_impl(sample_offset, port, data, None)
    }

    /// Atomically copy a `SysEx` payload and link its typed event to an exact
    /// payload. This is primarily used when rebasing a lossless event list.
    ///
    /// # Errors
    /// [`PushError::EventFull`], [`PushError::ExactEventFull`], or
    /// [`PushError::PoolFull`] when the corresponding bounded storage is full.
    pub fn try_push_sysex_with_exact_on_port(
        &mut self,
        sample_offset: u32,
        port: u8,
        data: &[u8],
        exact: ExactEvent,
    ) -> Result<(), PushError> {
        self.push_sysex_on_port_impl(sample_offset, port, data, Some(exact))
    }

    fn push_sysex_on_port_impl(
        &mut self,
        sample_offset: u32,
        port: u8,
        data: &[u8],
        exact: Option<ExactEvent>,
    ) -> Result<(), PushError> {
        let pool_offset = self.sysex_pool.len();
        self.reserve_event_slot()?;
        if exact.is_some() {
            self.reserve_exact_slot()?;
        }
        let Some(pool_end) = pool_offset.checked_add(data.len()) else {
            return self.fail(PushError::PoolFull);
        };
        if pool_end > self.sysex_pool.capacity() {
            return self.fail(PushError::PoolFull);
        }
        self.sysex_pool.extend_from_slice(data);
        // `as u32` casts are bounded: pool capacity is sized in the
        // hundreds of KiB at most, and the bounds check above keeps
        // `pool_offset + data.len()` under capacity, which itself
        // fits in `u32` by construction (`SYSEX_POOL_PREALLOC` ==
        // 128 KiB).
        #[allow(clippy::cast_possible_truncation)]
        let event = Event {
            sample_offset,
            port,
            body: EventBody::SysEx {
                pool_offset: pool_offset as u32,
                len: data.len() as u32,
            },
        };
        let event_index = self.events.len();
        let sequence = self.take_sequence();
        let exact_index = exact.as_ref().map(|_| self.exact_events.len());
        self.events.push(event);
        self.event_exact.push(exact_index);
        self.event_sequence.push(sequence);
        if let Some(mut exact) = exact {
            exact.sample_offset = sample_offset;
            self.exact_events.push(StoredExactEvent {
                event: exact,
                fallback_index: Some(event_index),
                sequence,
            });
        }
        Ok(())
    }

    /// Resolve a [`EventBody::SysEx`] entry to its payload bytes.
    /// Returns an empty slice for any other variant or an invalid range. The
    /// latter can only come from a manually constructed `EventBody::SysEx`;
    /// fail closed rather than panicking on the audio thread.
    #[must_use]
    pub fn sysex_bytes(&self, body: &EventBody) -> &[u8] {
        match body {
            EventBody::SysEx { pool_offset, len } => {
                let start = *pool_offset as usize;
                let Some(end) = start.checked_add(*len as usize) else {
                    return &[];
                };
                self.sysex_pool.get(start..end).unwrap_or(&[])
            }
            _ => &[],
        }
    }

    pub fn clear(&mut self) {
        self.events.clear();
        self.event_exact.clear();
        self.event_sequence.clear();
        self.exact_events.clear();
        // `Vec::clear` preserves capacity; the pool stays
        // pre-allocated for the next block.
        self.sysex_pool.clear();
        self.next_sequence = 0;
    }

    pub fn iter(&self) -> impl Iterator<Item = &Event> {
        self.events.iter()
    }

    /// Exact events in stable timestamp order after
    /// [`Self::ensure_sorted_by_offset`]. Linked entries expose their typed
    /// fallback; exact-only entries return `None` from
    /// [`ExactEventRef::fallback`].
    pub fn exact_iter(&self) -> impl Iterator<Item = ExactEventRef<'_>> {
        self.exact_events.iter().map(|stored| ExactEventRef {
            exact: &stored.event,
            fallback: stored
                .fallback_index
                .and_then(|index| self.events.get(index)),
        })
    }

    #[must_use]
    pub fn exact_get(&self, index: usize) -> Option<ExactEventRef<'_>> {
        let stored = self.exact_events.get(index)?;
        Some(ExactEventRef {
            exact: &stored.event,
            fallback: stored
                .fallback_index
                .and_then(|fallback| self.events.get(fallback)),
        })
    }

    /// Return the exact payload linked to a typed event index, if any.
    /// Adapters which keep their existing typed loop can use this to suppress
    /// the fallback when they emit the exact payload separately.
    #[must_use]
    pub fn exact_for_event(&self, event_index: usize) -> Option<ExactEventRef<'_>> {
        let exact_index = self.event_exact.get(event_index).copied().flatten()?;
        self.exact_get(exact_index)
    }

    /// Merge exact events with typed events lacking an exact payload without
    /// allocation. Call [`Self::ensure_sorted_by_offset`] first. Ties retain
    /// the original cross-lane insertion order.
    pub fn lossless_iter(&self) -> impl Iterator<Item = LosslessEventRef<'_>> {
        let mut event_index = 0;
        let mut exact_index = 0;

        core::iter::from_fn(move || {
            while self
                .event_exact
                .get(event_index)
                .copied()
                .flatten()
                .is_some()
            {
                event_index += 1;
            }

            let typed_key = self.events.get(event_index).and_then(|event| {
                self.event_sequence
                    .get(event_index)
                    .map(|sequence| (event.sample_offset, *sequence))
            });
            let exact_key = self
                .exact_events
                .get(exact_index)
                .map(|stored| (Self::exact_offset(stored, &self.events), stored.sequence));

            match (typed_key, exact_key) {
                (None, None) => None,
                (Some(_), None) => {
                    let event = self.events.get(event_index)?;
                    event_index += 1;
                    Some(LosslessEventRef::Typed(event))
                }
                (None, Some(_)) => {
                    let event = self.exact_get(exact_index)?;
                    exact_index += 1;
                    Some(LosslessEventRef::Exact(event))
                }
                (Some(typed), Some(exact)) if typed <= exact => {
                    let event = self.events.get(event_index)?;
                    event_index += 1;
                    Some(LosslessEventRef::Typed(event))
                }
                (Some(_), Some(_)) => {
                    let event = self.exact_get(exact_index)?;
                    exact_index += 1;
                    Some(LosslessEventRef::Exact(event))
                }
            }
        })
    }

    #[must_use]
    pub fn get(&self, index: usize) -> Option<&Event> {
        self.events.get(index)
    }

    #[must_use]
    pub fn len(&self) -> usize {
        self.events.len()
    }

    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.events.is_empty() && self.exact_events.is_empty()
    }

    #[must_use]
    pub fn exact_len(&self) -> usize {
        self.exact_events.len()
    }

    #[must_use]
    pub fn capacity(&self) -> usize {
        self.events.capacity()
    }

    #[must_use]
    pub fn exact_capacity(&self) -> usize {
        self.exact_events.capacity()
    }

    /// The most recent capacity failure, including failures ignored by the
    /// source-compatible [`Self::push`] API. This signal remains set across
    /// [`Self::clear`] until explicitly acknowledged.
    #[must_use]
    pub fn overflow(&self) -> Option<PushError> {
        self.overflow
    }

    pub fn clear_overflow(&mut self) {
        self.overflow = None;
    }

    /// Add `shift` to typed and exact timestamps appended at or after the
    /// supplied lane cursors. Saturation matches chunked processing's
    /// existing output-offset behaviour and never panics.
    pub fn shift_offsets_from(&mut self, event_from: usize, exact_from: usize, shift: u32) {
        for event in self.events.iter_mut().skip(event_from) {
            event.sample_offset = event.sample_offset.saturating_add(shift);
        }
        for stored in self.exact_events.iter_mut().skip(exact_from) {
            stored.event.sample_offset = stored.event.sample_offset.saturating_add(shift);
        }
    }

    /// Mutable access to the underlying typed event slice. Linked exact
    /// payloads inherit their fallback timestamp when observed, but callers
    /// shifting both lanes should use [`Self::shift_offsets_from`].
    #[doc(hidden)]
    pub fn events_mut(&mut self) -> &mut [Event] {
        &mut self.events
    }

    /// Current `SysEx` pool usage in bytes. Mainly useful in tests
    /// and for plug-in code that wants to surface "headroom
    /// remaining" in an editor.
    #[must_use]
    pub fn sysex_pool_used(&self) -> usize {
        self.sysex_pool.len()
    }

    /// Total `SysEx` pool capacity in bytes. Stable for the life of
    /// the `EventList` (no audio-thread reallocation).
    #[must_use]
    pub fn sysex_pool_capacity(&self) -> usize {
        self.sysex_pool.capacity()
    }

    fn reserve_event_slot(&mut self) -> Result<(), PushError> {
        if self.events.len() >= self.events.capacity()
            || self.event_exact.len() >= self.event_exact.capacity()
            || self.event_sequence.len() >= self.event_sequence.capacity()
        {
            return self.fail(PushError::EventFull);
        }
        Ok(())
    }

    fn reserve_exact_slot(&mut self) -> Result<(), PushError> {
        if self.exact_events.len() >= self.exact_events.capacity() {
            return self.fail(PushError::ExactEventFull);
        }
        Ok(())
    }

    fn fail<T>(&mut self, error: PushError) -> Result<T, PushError> {
        self.overflow = Some(error);
        Err(error)
    }

    fn take_sequence(&mut self) -> u64 {
        let sequence = self.next_sequence;
        self.next_sequence = self.next_sequence.saturating_add(1);
        sequence
    }

    fn exact_offset(stored: &StoredExactEvent, events: &[Event]) -> u32 {
        stored
            .fallback_index
            .and_then(|index| events.get(index))
            .map_or(stored.event.sample_offset, |event| event.sample_offset)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn push_sysex_round_trip() {
        let mut list = EventList::with_capacity(8);
        let payload = b"\x7E\x00\x06\x01"; // device-inquiry reply body
        list.push_sysex(42, payload).expect("pool has room");

        assert_eq!(list.len(), 1);
        let event = list.iter().next().expect("one event");
        assert_eq!(event.sample_offset, 42);
        assert!(matches!(event.body, EventBody::SysEx { .. }));
        assert_eq!(list.sysex_bytes(&event.body), payload);
        assert_eq!(list.sysex_pool_used(), payload.len());
    }

    #[test]
    fn push_sysex_two_messages_carve_pool_independently() {
        let mut list = EventList::with_capacity(8);
        let a = b"\x01\x02\x03";
        let b = b"\x04\x05\x06\x07";
        list.push_sysex(0, a).unwrap();
        list.push_sysex(1, b).unwrap();

        let collected: Vec<_> = list.iter().collect();
        assert_eq!(list.sysex_bytes(&collected[0].body), a);
        assert_eq!(list.sysex_bytes(&collected[1].body), b);
        assert_eq!(list.sysex_pool_used(), a.len() + b.len());
    }

    #[test]
    fn push_sysex_pool_full_is_recoverable() {
        // Construct a tiny pool by going through `with_capacity` with a
        // post-hoc shrink - we can't pass a custom pool size today, so
        // exercise the failure path by overflowing the configured 128 KiB.
        let mut list = EventList::with_capacity(8);
        let big = vec![0u8; SYSEX_POOL_PREALLOC];
        list.push_sysex(0, &big)
            .expect("first fill is exactly the pool");
        let err = list.push_sysex(1, b"\x00").unwrap_err();
        assert_eq!(err, PushError::PoolFull);
        // No partial state: the rejected event isn't queued, the pool
        // length is unchanged.
        assert_eq!(list.len(), 1);
        assert_eq!(list.sysex_pool_used(), SYSEX_POOL_PREALLOC);
    }

    #[test]
    fn clear_preserves_pool_capacity() {
        let mut list = EventList::with_capacity(8);
        let cap_before = list.sysex_pool_capacity();
        list.push_sysex(0, b"\x00\x01\x02").unwrap();
        list.clear();
        assert!(list.is_empty());
        assert_eq!(list.sysex_pool_used(), 0);
        // The whole point of pre-allocation: clearing must not free.
        assert_eq!(list.sysex_pool_capacity(), cap_before);
    }

    #[test]
    fn sort_preserves_sysex_offsets() {
        let mut list = EventList::with_capacity(8);
        let early = b"\x10\x11";
        let late = b"\x20\x21\x22";
        list.push_sysex(100, late).unwrap();
        list.push_sysex(0, early).unwrap();
        list.ensure_sorted_by_offset();

        let collected: Vec<_> = list.iter().collect();
        // Sorted: sample_offset=0 comes first, then 100.
        assert_eq!(collected[0].sample_offset, 0);
        assert_eq!(list.sysex_bytes(&collected[0].body), early);
        assert_eq!(collected[1].sample_offset, 100);
        assert_eq!(list.sysex_bytes(&collected[1].body), late);
    }

    #[test]
    fn sysex_bytes_returns_empty_for_non_sysex() {
        let list = EventList::with_capacity(8);
        let body = EventBody::NoteOn {
            group: 0,
            channel: 0,
            note: 60,
            velocity: 100,
        };
        assert!(list.sysex_bytes(&body).is_empty());
    }

    #[test]
    fn event_constructors_set_port() {
        let body = EventBody::NoteOn {
            group: 0,
            channel: 0,
            note: 60,
            velocity: 100,
        };
        assert_eq!(Event::new(10, body).port, 0);
        assert_eq!(Event::on_port(10, 4, body).port, 4);
    }

    #[test]
    fn push_sysex_on_port_stamps_port() {
        let mut list = EventList::with_capacity(8);
        list.push_sysex_on_port(0, 2, b"\x10\x11").unwrap();
        assert_eq!(list.iter().next().unwrap().port, 2);
    }

    #[test]
    fn ensure_sorted_orders_offsets_and_keeps_equal_offset_order() {
        let on = |ch: u8| EventBody::NoteOn {
            group: 0,
            channel: ch,
            note: 60,
            velocity: 100,
        };
        let mut list = EventList::with_capacity(8);
        // Per-event pushes at real offsets, then block-level pushes at
        // the last sample - the shape a vibrato-style emitter produces.
        list.push(Event::new(10, on(0)));
        list.push(Event::new(510, on(1)));
        list.push(Event::new(0, on(2)));
        list.push(Event::new(510, on(3))); // equal offset: must stay after ch 1
        list.ensure_sorted_by_offset();
        let order: Vec<(u32, u8)> = list
            .iter()
            .map(|e| match e.body {
                EventBody::NoteOn { channel, .. } => (e.sample_offset, channel),
                _ => unreachable!(),
            })
            .collect();
        assert_eq!(order, vec![(0, 2), (10, 0), (510, 1), (510, 3)]);
    }
}
