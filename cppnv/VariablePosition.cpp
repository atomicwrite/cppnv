﻿#include "VariablePosition.h"

variable_position::variable_position(const int variable_start, const int start_brace, const int dollar_sign):
    variable_start(
        variable_start), start_brace(start_brace), dollar_sign(dollar_sign), end_brace(0), variable_end(0)
{
}
