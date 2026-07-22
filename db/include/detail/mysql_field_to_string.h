#pragma once

#include <string>

namespace boost::mysql {
class field_view;
}

namespace hps::detail {

std::string mysql_field_to_string(const boost::mysql::field_view& field);

} // namespace hps::detail
