local input =
  {
    { "Mode", SOURCE},           -- user selects source (typically slider or knob)
    { "Trainer ", SOURCE},           -- user selects source (typically slider or knob)
    { "SWL ", SOURCE},           -- user selects source (typically slider or knob)
    { "SWH ", SOURCE},           -- user selects source (typically slider or knob)
    { "Mode1 ", SOURCE},           -- user selects source (typically slider or knob)
    { "Interval", VALUE, 0, 100, 0 } -- interval value, default = 0.
  }

local output = { "Val1", "Val2"  }

local function init()
  -- Called once when the script is loaded
end

local function run(Mode, Trainer, SWL, SWH, Mode1, Interval) -- Must match input table
  local v1 = 0
  local v2 = 0
  
  -- Called periodically
  return v1, v2                       -- Must match output table
end

return { input=input, output=output, run=run, init=init }