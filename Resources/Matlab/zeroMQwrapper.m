% 
% zeroMQwrapper provides a simple wrapper around the zeromq library with a
% order->response scenario in mind
%
% Usage:
% 
%      zeroMQwrapper('Command',...);
%
%      zeroMQwrapper('Send',url, order, [blocking=0]);
%           When blocking is true, zeroMQwrapper waits for and returns the response
%             [response, dialogue]=zeroMQwrapper('Send',url, order, 1);
%             When blocking is 0 (default) zeroMQwrapper adds the order to a queue and returns the time the order was added to that queue. See 'GetResponses'
%             [timeOrderAdded]=zeroMQwrapper('Send',url, order, 0);
%             
%      dialogue = zeroMQwrapper('GetResponses', [wairForEmptyQueue=1])   
%             Retirieves responses collected for orders send in non blocking mode. If wairForEmptyQueue>0, the function waits for the last queued order to get a response.
%             dialogue is a struct with the fields: order, response, timeOrderAdded, timeOrderSent, timeResponeRecieved
%             At the moment this cannot get filtered by the url of the order yet.
%             
%      zeroMQwrapper('CloseAll')   
%             closes all open sockets and the queue thread if open.
%        
% 
%      zeroMQwrapper('CloseThread')
%             closes the queue thread if open
%             
%      url = zeroMQwrapper('StartConnectThread', url)
%             This function is not neccessary and for backward compatability only. It simply returns the url of the input as previous versions required a handle instead of the url if the 'Send' command
%            
%   currently only tested on OSX. To compile you need the zeromq library
%   installed (e.g. using brew on OSX) and the libraries set correctly in
%   compile_matlab_wrapper
% 
% Copyright (c) 2008 Shay Ohayon, California Institute of Technology.
% This file is a part of a free software. you can redistribute it and/or modify
% it under the terms of the GNU General Public License as published by
% the Free Software Foundation (see GPL.txt)
%
% Changes to allow receiving responses by Jonas Knöll, 2016