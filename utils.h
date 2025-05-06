#ifndef __UTILS_H
#define __UTILS_H

#include "common.h"
#include <string>

int deserialize_std_string(const bytes *buf, std::string &s);
int serialize_std_string(bytes *buf, std::string &s);
#endif
