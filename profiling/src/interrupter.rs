use std::ptr::NonNull;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};

// TODO: support forking

#[derive(Debug, Eq, PartialEq)]
pub struct SignalPointers {
    pub wall_samples: NonNull<AtomicU32>,
    pub vm_interrupt: NonNull<AtomicBool>,
}

pub fn interrupter(
    signal_pointers: SignalPointers,
    wall_time_period_nanoseconds: u64,
) -> Interrupter {
    Interrupter::new(signal_pointers, wall_time_period_nanoseconds)
}

pub use crossbeam::Interrupter;

mod crossbeam {
    use super::*;
    use crate::thread_utils::{join_timeout, spawn};
    use crossbeam_channel::{bounded, select, tick, Sender, TrySendError};
    use libc::sched_yield;
    use log::{trace, warn};
    use std::thread::JoinHandle;
    use std::time::Duration;

    #[derive(Debug)]
    enum Message {
        Start,
        Stop,
        Pause,
        Shutdown,
    }

    pub struct Interrupter {
        sender: Sender<Message>,
        join_handle: Option<JoinHandle<()>>,
    }

    struct SendableSignalPointers {
        vm_interrupt: *const AtomicBool,
        wall_samples: *const AtomicU32,
    }

    impl From<SignalPointers> for SendableSignalPointers {
        fn from(pointers: SignalPointers) -> Self {
            Self {
                vm_interrupt: pointers.vm_interrupt.as_ptr(),
                wall_samples: pointers.wall_samples.as_ptr(),
            }
        }
    }

    impl From<&SendableSignalPointers> for SignalPointers {
        fn from(sendable: &SendableSignalPointers) -> Self {
            Self {
                vm_interrupt: unsafe { &*sendable.vm_interrupt }.into(),
                wall_samples: unsafe { &*sendable.wall_samples }.into(),
            }
        }
    }

    impl SendableSignalPointers {
        fn notify(&self) {
            let sp = SignalPointers::from(self);

            let wall_samples: &AtomicU32 = unsafe { sp.wall_samples.as_ref() };
            wall_samples.fetch_add(1, Ordering::SeqCst);

            // Trigger the VM interrupt after the others.
            let vm_interrupt: &AtomicBool = unsafe { sp.vm_interrupt.as_ref() };
            vm_interrupt.store(true, Ordering::SeqCst);
        }
    }

    /// # Safety
    /// Safe only because of the channel operations.
    unsafe impl Send for SendableSignalPointers {}

    impl Interrupter {
        const THREAD_NAME: &'static str = "ddprof_time";

        pub fn new(signal_pointers: SignalPointers, wall_time_period_nanoseconds: u64) -> Self {
            // > A special case is zero-capacity channel, which cannot hold
            // > any messages. Instead, send and receive operations must
            // > appear at the same time in order to pair up and pass the
            // > message over.
            let (sender, receiver) = bounded(0);
            let signal_pointers = SendableSignalPointers::from(signal_pointers);
            let join_handle = spawn(Self::THREAD_NAME, move || {
                let thread = std::thread::current();
                let thread_name = thread.name().unwrap_or("{unknown}");

                let wall_interval = Duration::from_nanos(wall_time_period_nanoseconds);
                let ticker = tick(wall_interval);
                let mut active = false;

                // The goal is to wait on ticker if the associated PHP thread
                // is serving a request. A tick or two after it ends may occur,
                // but if the PHP thread is idle, like if it's being kept open
                // with Connection: Keep-Alive, then we really need to limit
                // how much we wake up.

                // The code below has two select!s, where one of the recv's is
                // the same -- the code to handle receiving messages. The
                // difference is we only recv on the ticker if we're in an
                // `active` state.
                loop {
                    if active {
                        select! {
                            // Remember to duplicate any changes to the else
                            // branch below.
                            recv(receiver) -> message => match message {
                                Ok(Message::Start) => {
                                    active = true;
                                }
                                Ok(Message::Stop) => {
                                    active = false;
                                }
                                Ok(Message::Pause) => {
                                    std::thread::park();
                                }
                                _ => {
                                    break;
                                }
                            },

                            recv(ticker) -> message => match message {
                                Ok(_) => signal_pointers.notify(),

                                // How does a timer fail?
                                Err(err) => {
                                    warn!("{err:#}");
                                    break;
                                }
                            },
                        }
                    } else {
                        // It's not great that we duplicate this code, but it
                        // was the only way I could get it to compile.
                        select! {
                            recv(receiver) -> message => match message {
                                Ok(Message::Start) => {
                                    active = true;
                                }
                                Ok(Message::Stop) => {
                                    active = false;
                                }
                                Ok(Message::Pause) => {
                                    std::thread::park();
                                }
                                _ => {
                                    break;
                                }
                            },
                        }
                    }
                }

                trace!("thread {thread_name} shut down cleanly");
            });

            Self {
                sender,
                join_handle: Some(join_handle),
            }
        }

        #[cold]
        fn err<E>(err: E, context: String) -> anyhow::Error
        where
            E: std::error::Error + Send + Sync + 'static,
        {
            anyhow::Error::from(err).context(context)
        }

        pub fn start(&self) -> anyhow::Result<()> {
            self.sender
                .send(Message::Start)
                .map_err(|err| Self::err(err, format!("failed to start {}", Self::THREAD_NAME)))
        }

        pub fn stop(&self) -> anyhow::Result<()> {
            self.sender
                .send(Message::Stop)
                .map_err(|err| Self::err(err, format!("failed to stop {}", Self::THREAD_NAME)))
        }

        /// Try to pause the interrupter, which can be un-paused by calling
        /// [Interrupter::unpause].
        /// Note that this will not block, though it may yield the CPU core.
        pub fn try_pause(&self) -> anyhow::Result<()> {
            match self.sender.try_send(Message::Pause) {
                Err(e) if matches!(e, TrySendError::Disconnected(_)) => {
                    // If the channel is disconnected, time samples have already stopped.
                    Err(Self::err(
                        e,
                        format!("failed to pause {}: ", Self::THREAD_NAME),
                    ))
                }
                Err(_) => {
                    // It's not disconnected, so let's retry (but just once).
                    unsafe { sched_yield() };
                    self.sender.try_send(Message::Pause).map_err(|err| {
                        Self::err(err, format!("failed to pause {}", Self::THREAD_NAME))
                    })
                }
                Ok(_) => Ok(()),
            }
        }

        /// Unpauses a previously paused interrupter.
        pub fn unpause(&self) {
            if let Some(join_handle) = &self.join_handle {
                join_handle.thread().unpark();
            }
        }

        pub fn shutdown(&mut self) -> anyhow::Result<()> {
            let timeout = Duration::from_secs(2);
            self.sender
                .send_timeout(Message::Shutdown, timeout)
                .map_err(|err| {
                    Self::err(err, format!("failed to shutdown {}", Self::THREAD_NAME))
                })?;
            // todo: write impact
            let impact = "";

            if let Some(handle) = self.join_handle.take() {
                join_timeout(handle, Duration::from_secs(2), impact);
            }
            Ok(())
        }
    }
}
