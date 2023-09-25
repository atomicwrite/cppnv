#pragma once 

struct variable_position
{
    variable_position(int variable_start, int start_brace, int dollar_sign);
    int variable_start;
    int start_brace;
    int dollar_sign;
    int end_brace;
    int variable_end;
};
