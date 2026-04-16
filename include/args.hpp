#pragma once
#include <string>

struct ParsedArgs {
    bool decompress = false;
    std::string infile;
    std::string outfile;

    //parametry pouze pro kompresi
    //pri dekompresi je nutne nastaveni nacist ze souboru
    bool use_model = false;
    bool adaptive_scan = false;
    int  width = -1; 
};

ParsedArgs parse_arguments(int argc, char** argv);
