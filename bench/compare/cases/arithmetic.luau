return function(rounds: number, size: number, seed: number): number
    local total = 0
    for _ = 1, rounds do
        local sum = seed
        local i = 0
        while i < size do
            if i < seed then sum += i + 3 else sum += i + 1 end
            i += 1
        end
        total += sum
    end
    return total
end
