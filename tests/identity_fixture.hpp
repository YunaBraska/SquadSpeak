#pragma once

// Public synthetic TS3 export fixtures. Never use these keys on a real server.
// Fixed P-256 scalars with every byte 0x01 and 0x02, encoded as TS3 INI exports.

inline constexpr auto firstIdentityExport = R"ts([Identity]
identity="17Vb75V/5y2ZmPzY308/1sEip2dwANeDXl/cVVSZC1jW1gJUAB8W1BmAwJTJAZ4C3B1ElxHfXZoZy0jUn0GYn0JA39dZx91AUFkWBdyExlKVgtYAHNySwYDDkYzAV5WEV56H0ZVUEZFb0NJQUVCQVFFQkFRRUJBUUVCQVFFQkFRRUJBUUVCQVFFQkFRRUJBUUVCQVFFQg=="
nickname=Import test
)ts";
inline constexpr auto firstIdentityUid = "v+nJ5sCRFKPg67g2G5LXRw0YYEI=";

inline constexpr auto secondIdentityExport = R"ts([Identity]
identity="17VvXGAs22Mji/mUTC+8cBL7nekDzx0SWFWVQwxVzZ2R30JGG5OewVwCkgBEzVXUGViKXBceWpRWikvdH0GdwpzZgFGdUVhL0lKAjJIXSIWIg9LcRgFEFZyAGdLWm1QEXsHKF53AXVPWUNJQUlDQWdJQ0FnSUNBZ0lDQWdJQ0FnSUNBZ0lDQWdJQ0FnSUNBZ0lDQWdJQw=="
nickname=Import test
)ts";
inline constexpr auto secondIdentityUid = "36e5wEYlonTiF3pgtDJDlrsF17M=";
