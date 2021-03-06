#include <chrono>
#include <thread>
#include <memory>
#include <cstring>

#include "mpd_control.hpp"
#include "util.hpp"

#ifdef USE_POLL
#include <sys/eventfd.h>
#include <unistd.h>
#include <poll.h>
#endif

playlist_change_info::playlist_change_info(int nv, playlist_change_info::diff_type && cp, unsigned int l)
    : new_version(nv)
    , changed_positions(cp)
    , new_length(l)
{
}

mpd_control::mpd_control(std::function<void(std::optional<song_location>)> new_song_cb, std::function<void(bool)> random_cb, std::function<void()> playlist_changed_cb, std::function<void(mpd_state)> playback_state_changed_cb)
    : _c(mpd_connection_new(nullptr, 0, 0))
    , _run(true)
    , _new_song_cb(new_song_cb)
    , _random_cb(random_cb)
    , _playlist_changed_cb(playlist_changed_cb)
    , _playback_state_changed_cb(playback_state_changed_cb)
{
    if (mpd_connection_get_error(_c) != MPD_ERROR_SUCCESS)
        throw std::runtime_error(
            std::string("connecting to mpd failed:")
            + mpd_connection_get_error_message(_c)
        );
#ifdef USE_POLL
    _eventfd = eventfd(0, EFD_NONBLOCK);
#endif
}

mpd_control::~mpd_control()
{
    mpd_connection_free(_c);
#ifdef USE_POLL
    close(_eventfd);
#endif
}

void mpd_control::run()
{
    mpd_song * last_song = mpd_run_current_song(_c);

    new_song_cb(last_song);

    while (_run)
    {
        mpd_send_idle_mask(_c, static_cast<mpd_idle>(MPD_IDLE_PLAYER | MPD_IDLE_OPTIONS | MPD_IDLE_PLAYLIST));

#ifdef USE_POLL
        {
            // wake up if the thread signals it or the mpd server sends a
            // response
            struct pollfd pollfds[] =
                { { _eventfd, POLLIN, 0}
                , { mpd_connection_get_fd(_c), POLLIN, 0}
                };
            if (poll(pollfds, 2, -1) <= 0)
            {
                // error with poll, fall back to waiting
#endif
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
#ifdef USE_POLL
            }
            else
            {
                uint64_t dummy;
                eventfd_read(_eventfd, &dummy);
            }
        }
#endif

        enum mpd_idle idle_event = mpd_run_noidle(_c);
        if (idle_event & MPD_IDLE_PLAYER)
        {
            mpd_song * song = mpd_run_current_song(_c);

            if (last_song != nullptr)
            {
                if (song == nullptr || (song != nullptr && mpd_song_get_id(last_song) != mpd_song_get_id(song)))
                    new_song_cb(song);
                mpd_song_free(last_song);
            }
            else
            {
                if (song != nullptr)
                    new_song_cb(song);
            }
            last_song = song;
            {
                mpd_status * s = mpd_run_status(_c);
                _playback_state_changed_cb(mpd_status_get_state(s));
                mpd_status_free(s);
            }
        }
        if (idle_event & MPD_IDLE_OPTIONS)
        {
            mpd_status * s = mpd_run_status(_c);
            _random_cb(mpd_status_get_random(s));
            mpd_status_free(s);
        }
        {
            scoped_lock lock(_external_tasks_mutex);

            while (!_external_tasks.empty())
            {
                _external_tasks.front()(_c);
                _external_tasks.pop_front();
            }
        }
        if (idle_event & MPD_IDLE_PLAYLIST)
        {
            _playlist_changed_cb();
        }
        {
            scoped_lock lock(_external_song_queries_mutex);

            while (!_external_song_queries.empty())
            {
                _external_song_queries.front()(_c, last_song);
                _external_song_queries.pop_front();
            }
        }
    }
    if (last_song != nullptr)
        mpd_song_free(last_song);
}

void mpd_control::stop()
{
    _run = false;
#ifdef USE_POLL
    // wake up from poll
    eventfd_write(_eventfd, 1);
#endif
}

void mpd_control::toggle_pause()
{
    add_external_task([](mpd_connection * c){ mpd_run_toggle_pause(c); });
}

void mpd_control::inc_volume(unsigned int amount)
{
    add_external_task([amount](mpd_connection * c)
    {
        mpd_status * s = mpd_run_status(c);
        int new_volume = std::min(100, mpd_status_get_volume(s) + static_cast<int>(amount));
        mpd_run_set_volume(c, new_volume);
    });
}

void mpd_control::dec_volume(unsigned int amount)
{
    add_external_task([amount](mpd_connection * c)
    {
        mpd_status * s = mpd_run_status(c);
        int new_volume = std::max(0, mpd_status_get_volume(s) - static_cast<int>(amount));
        mpd_run_set_volume(c, new_volume);
    });
}

void mpd_control::next_song()
{
    add_external_task([](mpd_connection * c){ mpd_run_next(c); });
}

void mpd_control::prev_song()
{
    add_external_task([](mpd_connection * c){ mpd_run_previous(c); });
}

void mpd_control::play_position(int pos)
{
    add_external_task([pos](mpd_connection * c)
    {
        if (!mpd_run_play_pos(c, pos))
        {
            mpd_connection_clear_error(c);
        }
    });
}

void mpd_control::set_random(bool value)
{
    add_external_task([value](mpd_connection * c) { mpd_run_random(c, value); });
}

bool mpd_control::get_random()
{
    return add_external_task_with_return<bool>([](mpd_connection * c){
        mpd_status * s = mpd_run_status(c);
        bool v = mpd_status_get_random(s);
        mpd_status_free(s);
        return v;
    });
}

std::string mpd_control::get_current_title()
{
    return get_current_tag(MPD_TAG_TITLE);
}

std::string mpd_control::get_current_artist()
{
    return get_current_tag(MPD_TAG_ARTIST);
}

std::string mpd_control::get_current_album()
{
    return get_current_tag(MPD_TAG_ALBUM);
}

std::string format_playlist_song(mpd_song * s)
{
    char const * artist = mpd_song_get_tag(s, MPD_TAG_ARTIST, 0);
    if (artist == nullptr)
        artist = mpd_song_get_tag(s, MPD_TAG_ALBUM_ARTIST, 0);
    if (artist == nullptr)
        artist = mpd_song_get_tag(s, MPD_TAG_COMPOSER, 0);
    if (artist != nullptr && std::strcmp(artist, "Various Artists") == 0)
        artist = nullptr;

    return (artist == nullptr ? "" : std::string(artist) + " - ")
           + string_from_ptr(mpd_song_get_tag(s, MPD_TAG_TITLE, 0));
}

std::pair<std::vector<std::string>, unsigned int> mpd_control::get_current_playlist()
{
    typedef std::pair<std::vector<std::string>, unsigned int> result_type;

    return add_external_task_with_return<result_type>([](mpd_connection * c)
    {
        mpd_status * status = mpd_run_status(c);
        std::vector<std::string> playlist;
        playlist.reserve(mpd_status_get_queue_length(status));

        mpd_send_list_queue_meta(c);

        mpd_song * song;
        while ((song = mpd_recv_song(c)) != nullptr)
        {
            playlist.push_back(format_playlist_song(song));
            mpd_song_free(song);
        }

        auto version = mpd_status_get_queue_version(status);
        mpd_status_free(status);
        return std::make_pair(playlist, version);
    });
}

playlist_change_info mpd_control::get_current_playlist_changes(unsigned int version)
{
    return add_external_task_with_return<playlist_change_info>([version](mpd_connection * c)
    {

        mpd_status * status = mpd_run_status(c);
        playlist_change_info::diff_type changed_positions;

        mpd_send_queue_changes_meta(c, version);

        mpd_song * song;
        while ((song = mpd_recv_song(c)) != nullptr)
        {
            changed_positions.emplace_back(mpd_song_get_pos(song), format_playlist_song(song));
            mpd_song_free(song);
        }

        auto qv = mpd_status_get_queue_version(status);
        auto ql = mpd_status_get_queue_length(status);
        mpd_status_free(status);
        return playlist_change_info(qv, std::move(changed_positions), ql);
    });
}

std::string mpd_control::get_current_tag(enum mpd_tag_type type)
{
    typedef std::promise<std::string> promise_type;
    auto promise_ptr = std::make_shared<promise_type>();

    {
        scoped_lock lock(_external_song_queries_mutex);
        _external_song_queries.push_back([type, promise_ptr](mpd_connection * c, mpd_song * s)
        {
            if (s != nullptr)
            {
                char const * const cstr = mpd_song_get_tag(s, type, 0);
                promise_ptr->set_value(string_from_ptr(cstr));
            }
            else
            {
                promise_ptr->set_value("");
            }

        }
        );
    }

#ifdef USE_POLL
    eventfd_write(_eventfd, 1);
#endif

    return promise_ptr->get_future().get();
}

void mpd_control::add_external_task(std::function<void(mpd_connection *)> t)
{
    scoped_lock lock(_external_tasks_mutex);
    _external_tasks.push_back(t);
#ifdef USE_POLL
    eventfd_write(_eventfd, 1);
#endif
}

void mpd_control::new_song_cb(mpd_song * s)
{
    _new_song_cb(s != nullptr ? std::make_optional(song_location{ mpd_song_get_uri(s), mpd_song_get_pos(s) }) : std::nullopt);
}
