//! Event-driven outbound file drags for the baseview editor.
//!
//! The request is queued in the egui context by plugin UI code and consumed by
//! the owning baseview event loop. No process-global queue or audio-thread
//! access is involved.

use std::path::PathBuf;

use egui::Id;
use matari_audio_drag_and_drop::{Outcome, SessionRoute};

fn request_id() -> Id {
    Id::new("truce-egui-external-drag-request")
}

fn events_id() -> Id {
    Id::new("truce-egui-external-drag-events")
}

#[derive(Clone, Debug, Default)]
struct Request(Vec<PathBuf>);

/// Lifecycle event delivered by a native outbound drag session.
#[derive(Clone, Debug, PartialEq, Eq)]
#[non_exhaustive]
pub enum Event {
    /// Native protocol committed the drag.
    Started { session: u64, route: SessionRoute },
    /// The target requested the file payload.
    DataRequested { session: u64 },
    /// Native protocol observed the target drop.
    DropPerformed { session: u64 },
    /// Native protocol reached one terminal result.
    Terminal { session: u64, outcome: Outcome },
    /// The request could not be validated or scheduled.
    Failed { message: String },
}

/// Request an outbound drag for a validated export file.
///
/// The actual native drag starts only from the editor's GUI event loop while
/// the initiating pointer gesture is still owned by baseview.
///
/// # Errors
///
/// Returns an error when `path` is not absolute.
pub fn request_file(context: &egui::Context, path: PathBuf) -> Result<(), &'static str> {
    if !path.is_absolute() {
        return Err("outbound drag requires an absolute file path");
    }
    context.data_mut(|data| data.insert_temp(request_id(), Request(vec![path])));
    context.request_repaint();
    Ok(())
}

/// Drain lifecycle events observed since the previous frame.
#[must_use]
pub fn drain_events(context: &egui::Context) -> Vec<Event> {
    context.data_mut(|data| std::mem::take(data.get_temp_mut_or_default::<Vec<Event>>(events_id())))
}

pub(crate) fn take_request(context: &egui::Context) -> Option<Vec<PathBuf>> {
    context
        .data_mut(|data| data.remove_temp::<Request>(request_id()))
        .map(|request| request.0)
}

pub(crate) fn push_event(context: &egui::Context, event: Event) {
    context.data_mut(|data| {
        data.get_temp_mut_or_default::<Vec<Event>>(events_id())
            .push(event);
    });
    context.request_repaint();
}
