#pragma once
#include <string>

class env_key
{
public:
    std::string* key;
    /**
     * \brief The current index in the buffer key
     */
    int key_index = 0;

    ~env_key()
    {
        delete key;
    }
};
