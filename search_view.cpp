#include <unicode/unistr.h>

#include "song_list_view.hpp"

#include "search_view.hpp"

search_view::search_view(vec size, std::string keys, std::vector<std::string> const & values, std::function<void(std::size_t)> activate_callback)
    : search_view( std::make_shared<keypad>(size, keys, [=](auto str){ on_submit(str); })
            , std::make_shared<list_view>(_filtered_values, 0, [=](auto idx){ activate_callback(this->_filtered_indices[idx]); })
            , values
            )
{
}

search_view::search_view(std::shared_ptr<keypad> keypad, std::shared_ptr<list_view> list_view, std::vector<std::string> const & values)
    : embedded_widget<notebook>(std::vector<widget_ptr>{ keypad, song_list_view(list_view, "⌨", [this](){ on_back(); }) })
    , _keypad(keypad)
    , _list_view(list_view)
    , _values(values)
{
}

void search_view::on_submit(std::string search_term)
{
    for (std::size_t pos = 0; pos < _values.get().size(); pos++)
    {
        auto const & s = _values.get()[pos];
        if (icu::UnicodeString::fromUTF8(s).toLower().indexOf(icu::UnicodeString::fromUTF8(search_term)) != -1)
        {
            _filtered_indices.push_back(pos);
            _filtered_values.push_back(s);
        }
    }
    _embedded_widget.set_page(1);
}

void search_view::on_back()
{
    _embedded_widget.set_page(0);
}


void search_view::on_playlist_changed()
{
    _list_view->set_position(0);
    on_back();
}
