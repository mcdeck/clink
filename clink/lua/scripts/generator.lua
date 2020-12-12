-- Copyright (c) 2015 Martin Ridgers
-- License: http://opensource.org/licenses/MIT

--------------------------------------------------------------------------------
clink = clink or {}
local _generators = {}
local _generators_unsorted = false

--------------------------------------------------------------------------------
-- Deprecated.
local _current_builder = nil



--------------------------------------------------------------------------------
local function prepare()
    -- Sort generators by priority if required.
    if _generators_unsorted then
        local lambda = function(a, b) return a._priority < b._priority end
        table.sort(_generators, lambda)

        _generators_unsorted = false
    end
end

--------------------------------------------------------------------------------
function clink._generate(line_state, match_builder)
    local impl = function ()
        for _, generator in ipairs(_generators) do
            local ret = generator:generate(line_state, match_builder)
            if ret == true then
                return true
            end
        end

        return false
    end

    prepare()
    _current_builder = match_builder

    local ok, ret = xpcall(impl, _error_handler_ret)
    if not ok then
        print("")
        print("match generator failed:")
        print(ret)
        return
    end

    _current_builder = nil
    return ret or false
end

--------------------------------------------------------------------------------
function clink._get_word_break_info(line_state)
    local impl = function ()
        local truncate = 0
        local keep = 0
        for _, generator in ipairs(_generators) do
            if generator.getwordbreakinfo then
                local t, k = generator:getwordbreakinfo(line_state)
                t = t or 0
                k = k or 0
                if (t > truncate) or (t == truncate and k > keep) then
                    truncate = t
                    keep = k
                end
            end
        end

        return truncate, keep
    end

    prepare()

    local ok, ret1, ret2 = xpcall(impl, _error_handler_ret)
    if not ok then
        print("")
        print("getting word break info failed:")
        print(ret1)
        return
    end

    return ret1, ret2
end

--------------------------------------------------------------------------------
--- -name:  clink.generator
--- -arg:   [priority:integer]
--- -ret:   table
--- Creates and returns a new match generator object.  Define on the object a
--- <code>generate()</code> function which gets called in increasing
--- <em>priority</em> order (low values to high values) when generating matches
--- for completion.  See <a
--- href="#matchgenerators">Match Generators</a> for more information.
function clink.generator(priority)
    if priority == nil then priority = 999 end

    local ret = { _priority = priority }
    table.insert(_generators, ret)

    _generators_unsorted = true
    return ret
end

--------------------------------------------------------------------------------
--- -name:  clink.add_match
--- -arg:   match:string
--- -ret:   nil
--- -deprecated: builder:addmatch
--- This is a shim that lets clink.register_match_generator continue to work
--- for now, despite being obsolete.
function clink.add_match(match)
    if _current_builder then
        _current_builder:addmatch(match)
    end
end

--------------------------------------------------------------------------------
--- -name:  clink.is_match
--- -arg:   needle:string
--- -arg:   candidate:string
--- -ret:   true
--- -deprecated: clink.generator
--- This is no longer needed, and simply returns true now.
function clink.is_match(needle, candidate)
    return true
end

--------------------------------------------------------------------------------
--- -name:  clink.matches_are_files
--- -arg:   [files:boolean]
--- -deprecated: builder:addmatch
--- This is no longer needed, because now it's inferred from the match type when
--- adding matches.
function clink.matches_are_files(files)
    if _current_builder then
        _current_builder:setmatchesarefiles(files)
    end
end

--------------------------------------------------------------------------------
--- -name:  rl_state
--- -var:   table
--- -deprecated: line
--- This is an obsolete global variable that was set while running match
--- generators.  Its table scheme is <em>{
--- line_buffer:string, point:integer}</em>, but it has been superseded by the
--- <a href="#line">line</a> type parameter passed into match generator
--- functions when using the new <a href="#clink.generator">clink.generator</a>
--- API.

--------------------------------------------------------------------------------
--- -name:  clink.register_match_generator
--- -arg:   func:function
--- -arg:   priority:integer
--- -show:  -- Deprecated form:
--- -show:  local function match_generator_func(text, first, last, match_builder)
--- -show:  &nbsp; -- `text` is the line text.
--- -show:  &nbsp; -- `first` is the index of the beginning of the end word.
--- -show:  &nbsp; -- `last` is the index of the end of the end word.
--- -show:  &nbsp; -- `clink.add_match()` is used to add matches.
--- -show:  &nbsp; -- return true if handled, or false to let another generator try.
--- -show:  end
--- -show:  clink.register_match_generator(match_generator_func, 10)<br/>
--- -show:  -- Replace with new form:
--- -show:  local g = clink.generator(10)
--- -show:  function g:generate(line_state, match_builder)
--- -show:  &nbsp; -- `line_state` is a <a href="#line">line</a> object.
--- -show:  &nbsp; -- `match_builder:<a href="#builder:addmatch">addmatch</a>()` is used to add matches.
--- -show:  &nbsp; -- return true if handled, or false to let another generator try.
--- -show:  end
--- -deprecated: clink.generator
--- Registers a generator function for producing matches.  This behaves
--- similarly to v0.4.8, but not identically.  The Clink schema has changed
--- significantly enough that there is no direct 1:1 translation; generators are
--- called at a different time than before and have access to more information
--- than before.
function clink.register_match_generator(func, priority)
    local g = clink.generator(priority)
    function g:generate(line_state, match_builder)
        local text = line_state:getendword()
        local info = line_state:getwordinfo(line_state:getwordcount())
        local first = info.offset
        local last = first + info.length - 1
        -- // TODO: adjust_for_separator()?

        return func(text, first, last)
    end
end
