# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__all__ = [
    "download_available_updates_list",
    "downloader_status",
    "DownloadStatus",
]

import enum
import logging
from pathlib import Path
from typing import Callable

import bpy
from _bpy_internal.http import downloader as http_dl

logger = logging.getLogger(__name__)


class BlenderUpdatesListing:
    downloader: UpdatesDownloader | None = None


_blender_updates_listing: BlenderUpdatesListing = BlenderUpdatesListing()


def download_available_updates_list() -> None:
    if _blender_updates_listing.downloader is None:
        _blender_updates_listing.downloader = UpdatesDownloader(
            on_download_done_callback,
            on_done_callback,
        )
        _blender_updates_listing.downloader.start()
        _blender_updates_listing.downloader.download_available_updates_list_file()


def download_available_updates_if_expired() -> None:
    from _bpy_internal.available_updates import blender_updates as updates
    if updates.is_cache_expired():
        download_available_updates_list()


def on_download_done_callback(
    downloader: UpdatesDownloader,
    _http_req_descr: http_dl.RequestDescription,
    _temp_path: Path,
) -> None:
    try:
        with _temp_path.open() as file:
            from _bpy_internal.available_updates import blender_updates as releases
            releases.load_json_updates_to_tracks(file.read())
    finally:
        pass


def on_done_callback(
    downloader: UpdatesDownloader,
) -> None:
    if downloader.status == DownloadStatus.FINISHED:
        bpy.types.WindowManager.check_for_available_updates_status_finished_loading()
    else:
        bpy.types.WindowManager.check_for_available_updates_status_failed_loading()
    _blender_updates_listing.downloader = None


def downloader_status() -> DownloadStatus:
    """Returns the downloader status.

    Raises a NoneType error if there is no active download.
    """
    return _blender_updates_listing.downloader.status


class DownloadStatus(enum.Enum):
    IDLE = 'idle'
    DOWNLOADING = 'downloading'

    FINISHED = 'finished'
    """The downloader has downloaded the list of latest available releases.
    """

    FAILED = 'failed'
    """Unexpected exceptions occurred."""


class UpdatesDownloader:

    # Called when the entire queue is 'done':
    type OnDoneCallback = Callable[['UpdatesDownloader'], None]
    _on_done_callback: OnDoneCallback

    # Called for each downloaded file being 'done':
    type OnDownloadDoneCallback = Callable[['UpdatesDownloader', http_dl.RequestDescription, Path], None]
    _on_download_done_callback: OnDownloadDoneCallback | None

    _bg_downloader: http_dl.BackgroundDownloader | None

    _status: DownloadStatus
    _error_message: str
    """An error message to show to the user.

    Should be set on errors to communicate a message to users. Calling report()
    with 'ERROR' as the level will set this to the given message.
    """

    _DOWNLOAD_POLL_INTERVAL: float = 0.01
    """How often the background download process is polled, in seconds.

    Each 'poll' involves sending queued messages back & forth between the main
    Blender process and the background download process.
    """
    _temp_dir = None

    def __init__(
        self,
        on_download_done_callback: OnDownloadDoneCallback,
        on_done_callback: OnDoneCallback,
    ) -> None:
        """Create a downloader for checking the latest available updates.

        :param on_done_callback: called with one parameter (this
            UpdatesDownloader) whenever the downloader is "done".

            Here "done" does not imply "successful", as cancellations, network
            errors, or other issues can cause things to abort. In that case,
            this function is still called.

        :param on_download_done_callback: called with one parameter (this
            UpdatesDownloader) when the list of latest releases has
            finished downloading and was put in its final location, ready to be
            picked up.
        """

        self._on_download_done_callback = on_download_done_callback
        self._on_done_callback = on_done_callback

        self._status = DownloadStatus.IDLE
        self._error_message = ""

        # Work around a limitation of Blender, see bug report #139720 for details.
        self.on_timer_event = self.on_timer_event  # type: ignore[method-assign]

        import tempfile
        self._temp_dir = tempfile.TemporaryDirectory()

        from pathlib import Path
        output_dir = Path(self._temp_dir.name)

        self._http_metadata_provider = http_dl.MetadataProviderFilesystem(
            cache_location=output_dir / "http_metadata",
        )

        self._bg_downloader = None

    def _create_bg_downloader(self) -> None:
        self._bg_downloader = http_dl.BackgroundDownloader(
            options=http_dl.DownloaderOptions(
                metadata_provider=self._http_metadata_provider,
                timeout=300,
                http_headers={
                    'X-Blender': "{:d}.{:d}".format(*bpy.app.version),
                },
            ),
            on_callback_error=self._on_callback_error,
        )
        self._bg_downloader.add_reporter(self)

    def start(self) -> None:
        """Start the background process."""
        if not self._bg_downloader:
            self._create_bg_downloader()
            assert self._bg_downloader
        self._bg_downloader.start()

        # Register the timer for periodic message passing between the main and
        # background processes.
        if not bpy.app.timers.is_registered(self.on_timer_event):
            bpy.app.timers.register(
                self.on_timer_event,
                first_interval=self._DOWNLOAD_POLL_INTERVAL,
                persistent=True,
            )
            # Double-check the registration worked, see #139720 for details.
            assert bpy.app.timers.is_registered(self.on_timer_event)

    def download_available_updates_list_file(self) -> None:
        """Download the list of latest available release updates to a temp file."""

        # If the downloader was shut down, start it up again.
        if not self._bg_downloader:
            self.start()
        import platform
        query_params = {}
        query_params["platform"] = "{:s}-{:s}".format(bpy.app.build_platform.decode("utf-8"), platform.machine())
        query_params["blender_version"] = bpy.app.version_string
        query_params["cycle"] = bpy.app.version_cycle

        import urllib.parse
        url: str = "http://download.blender.org/api/releases/?" + urllib.parse.urlencode(query_params)
        save_to: Path = Path(self._temp_dir.name) / "updates.json"
        self._status = DownloadStatus.DOWNLOADING
        self._queue_download(url, save_to)

    def _shutdown_if_done(self) -> None:
        self.shutdown(DownloadStatus.FINISHED)

    def _on_callback_error(
            self,
            http_req_descr: http_dl.RequestDescription,
            local_file: Path,
            exception: Exception) -> None:
        logger.exception(
            "exception while handling downloaded file ({!r}, saved to {!r})".format(
                http_req_descr, local_file))
        self.report({'ERROR'}, "Resource download had an issue, download aborted")
        self.shutdown(DownloadStatus.FAILED)
        bpy.types.WindowManager.check_for_available_updates_status_failed_loading()

    def _queue_download(self, url: str, download_to_path: Path | str) -> Path:
        """Queue up this download, returning the path to which it will be downloaded."""

        logger.info("downloading %s to %s", url, download_to_path)

        assert self._bg_downloader, "downloads can only be queued when the bgdownloader is available"

        assert self._bg_downloader.num_pending_downloads == 0, "there is no need to download more than once the available updates list file"

        self._bg_downloader.queue_download(
            url,
            download_to_path,
            self._ond_download_done_done,
        )
        return download_to_path

    def _ond_download_done_done(self,
                                http_req_descr: http_dl.RequestDescription,
                                local_file: Path,
                                ) -> None:
        if self._on_download_done_callback:
            self._on_download_done_callback(self, http_req_descr, local_file)

    # TODO: implement this in a more useful way:
    def report(self, level: set[str], message: str) -> None:
        # logger.info("Report: {:s}: {:s}".format("/".join(level), message))
        if 'ERROR' in level:
            self._error_message = message

    def shutdown(self, status: DownloadStatus) -> None:
        """Stop the background downloader, update the status and call the 'done' callback."""
        self._status = status

        # The timer is no longer necessary, the bg_downloader.shutdown() call
        # takes care of the last queued messages.
        if bpy.app.timers.is_registered(self.on_timer_event):
            bpy.app.timers.unregister(self.on_timer_event)

        # Cleanup temp directory
        if self._temp_dir:
            self._temp_dir.cleanup()
            self._temp_dir = None

        try:
            if not self._bg_downloader:
                return

            # Only report if this is actually triggering a shutdown. If that was
            # already triggered somehow, don't bother.
            if not self._bg_downloader.is_shutdown_requested:
                # It may be tempting to call self.report(...) here, and report on the
                # cancellation. However, this should be done by the caller, when they know
                # of the reason of the cancellation and thus can provide more info.
                num_pending = self._bg_downloader.num_pending_downloads
                if num_pending:
                    logger.warning("Shutting down background downloader, %d downloads pending", num_pending)

            self._bg_downloader.shutdown()
        finally:
            # Regardless of whether the shutdown had some issues, the timer has
            # been unregistered, so there will be no more message handling, and
            # so for all intents and purposes, the downloader is done.
            self._bg_downloader = None
            self._on_done_callback(self)

    def on_timer_event(self) -> float:
        assert self._bg_downloader, "timer events should only come in while the bgdownloader is available"

        try:
            self._bg_downloader.update()
        except http_dl.BackgroundProcessNotRunningError:
            logger.error("Background downloader subprocess died, aborting.")
            self.shutdown(DownloadStatus.FAILED)
            return 0  # Deactivate the timer.
        except Exception:
            logger.exception(
                "Unexpected error downloading new blender updates available list")

        # Automatically switch between IDLE and DOWNLOADING, but never overwrite
        # FAILED or FINISHED_SUCCESFULLY.
        if self._status in {DownloadStatus.DOWNLOADING, DownloadStatus.IDLE}:
            if self._bg_downloader.num_pending_downloads > 0:
                self._status = DownloadStatus.DOWNLOADING
            else:
                self._status = DownloadStatus.IDLE

        return self._DOWNLOAD_POLL_INTERVAL

    @property
    def status(self) -> DownloadStatus:
        return self._status

    @property
    def error_message(self) -> str:
        return self._error_message

    def download_starts(self, http_req_descr: http_dl.RequestDescription) -> None:
        logger.debug("Download starting: %s", http_req_descr)

    def already_downloaded(
        self,
        http_req_descr: http_dl.RequestDescription,
        local_file: Path,
    ) -> None:
        self._shutdown_if_done()

    def download_error(
        self,
        http_req_descr: http_dl.RequestDescription,
        local_file: Path,
        error: Exception,
    ) -> None:
        logger.warning("Could not download file %s: %s", http_req_descr, error)
        self.shutdown(DownloadStatus.FAILED)

    def download_progress(
        self,
        http_req_descr: http_dl.RequestDescription,
        progress: http_dl.DownloadProgress,
    ) -> None:
        pass

    def download_finished(
        self,
        http_req_descr: http_dl.RequestDescription,
        local_file: Path,
    ) -> None:
        _blender_updates_listing.downloader = None
        logger.info("Download finished: %s", http_req_descr)
        self._shutdown_if_done()
