local function fib(n)
    if n < 2 then return n end
    return fib(n - 1) + fib(n - 2)
end
return function(rounds, size, seed)
    local total = 0
    for _ = 1, rounds do
        local i = 0
        while i < size do
            total = total + fib(seed)
            i = i + 1
        end
    end
    return total
end
