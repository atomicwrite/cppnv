# CPP ENV

## A cpp .env interpreter

`.env` files (dotenv) are key-value pairs that evolved from bash environment exports.

```bash
api_key=abc123
bugger=purse
```

## Usage

You pass in an istream it can read from that 
supports eof and good. Considering also adding wide istream for unicode.
```c++
   std::vector<env_pair*> env_pairs;
   env_reader::read_pairs(some_istream, &env_pairs);

```

To interpolate you iterate over which pairs you want to
interpolate. The code will auto interpolate pairs if they are required for replacement

As of writing this I haven't optimized the interpolate function to
figure out the size it needs for it's final replacement I'm using
.replace to get it running. This only affects you if you have 100s of interpolations.
At 1-5 in a single variable it's nothing. I haven't yet because I wonder
if it's one of those optimization (since it has to scan the array twice to avoid resizing the string)
that might not be opt because most of the usage is 1-3 variables and most of the time 0. When there are 0 variables
the string is just copied. 
```c++

  for (const auto pair : env_pairs)
    {
 
        env_reader::finalize_value(pair, &env_pairs);
        std::cout << *(pair->key->key) << " = |" << *(pair->value->value) << "|" <<std::endl;
    }

```

## Variable Names (Tested)

We allow just about anything other than a newline. Which means
yes if you want to have `a\tb=some_value` you can. You can not however have a space in the
variable name.

## Values

Values are interpolated against each other and checked for circular.
### Variables (Tested)

```bash
one=1
two=${one}
three=hmm $ {two }
four=${ two } * ${ three}


````

### Heredocs  (Tested)
You can have double quoted heredocs
```bash
some_heredoc="""

hey wow new lines are preserved

"""
```
### Heredoc variables (Tested)
```bash
some_heredoc="""

hey wow new lines are preserved

${some_heredoc_variable} 

"""
some_heredoc_variable="""

I like cheese

"""
```
### Single Quote Heredoc (In progress)

Single quote heredocs aren't interpreted nor are it's control codes replaced similar to single quoted strings
```bash
nope='${yes}'
yes=no
```


### Control Codes (In progress)

* \n Linefeed 
* \r Carriage return
* \t Tab
* \f Form feed
* \b Backspace
* \" Double-quote
* \' Single-quote
* \\ Backslash


## Caveats

I'm not deleting the new strings I make yet. I use a single string as a buffer when reading in all the pairs
as it reduces the resizes on strings. That string is then replaced when the key or value exits it's read. On
calling finalize, if the string has variables it might or might not as mentioned above be optimal to count
the displacement of string size the variables would cause over the placeholder and resize the string once and copy in.
