local function fib(n: number): number
    if n < 2 then return n end
    return fib(n - 1) + fib(n - 2)
end
return function(rounds: number, size: number, seed: number): number
    local total = 0
    for _ = 1, rounds do
        local i = 0
        while i < size do
            total += fib(seed)
            i += 1
        end
    end
    return total
end
