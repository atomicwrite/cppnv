#pragma once
#include <string>
#include <vector>

#include "VariablePosition.h"


struct env_value
{
    std::string* value;
    bool is_parsing_variable = false;
    std::vector<variable_position*>* interpolations;
    int interpolation_index = 0;
    bool quoted = false;
    bool triple_quoted = false;
    bool double_quoted = false;
    bool triple_double_quoted = false;
    int value_index = 0;
    bool is_already_interpolated = false;
    bool is_being_interpolated = false;
    bool did_over_flow = false;
    int back_slash_streak = 0;
    int single_quote_streak = 0;
    int double_quote_streak = 0;

    env_value(): value(nullptr)
    {
        interpolations = new std::vector<variable_position*>();
    }

    ~env_value()
    {
        for (const auto interpolation : *interpolations)
        {
            delete interpolation;
        }
        delete value;
        delete interpolations;
    }
};
